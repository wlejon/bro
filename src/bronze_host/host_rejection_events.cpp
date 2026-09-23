// `unhandledrejection` / `rejectionhandled`: HTML's "notify about rejected
// promises" (§8.1.5.6) over bronze's rejection hook (embed.h
// setPromiseRejectionHook).
//
// bronze calls the hook at the end of every microtask drain, once per
// promise that was rejected with nothing subscribed and still has nothing —
// that is the checkpoint the spec runs its notify step at — and again, as
// Handled, when a promise it reported later gets its first handler. What the
// spec does with those two facts is here:
//
//   Unhandled  the promise joins the "about to be notified" list and a task is
//              queued. The task skips a promise that was handled in the
//              meantime (a `.catch` attached in a later microtask of the same
//              turn is not a rejection nobody handled), and fires a cancelable
//              PromiseRejectionEvent `unhandledrejection` at the global for
//              each of the rest: `onunhandledrejection` first (returning false
//              cancels, as every event-handler attribute bar onerror does),
//              then the listeners. An uncancelled one is reported the way an
//              uncaught exception is — the engine log line — and, under the
//              headless driver, fails the run.
//   Handled    a promise whose `unhandledrejection` has already FIRED gets a
//              non-cancelable `rejectionhandled` as a later task. One handled
//              before its task ran just drops out of the list: the page never
//              heard it was unhandled, so there is nothing to take back.
//
// The event fires at the main window (a queued host task, drained at the top
// of the frame seam) and at a worker's `self` (the worker loop flushes after
// each of its own drains). One implementation serves both: the hook is per
// thread, as bronze's runtime is, and so is the state behind it.

#include "bronze_host/host_rejection_events.h"
#include "bronze_host/host_headless.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "util/log.h"

#include <utility>
#include <vector>

namespace bro::bronze_host {

namespace {

struct RejectionItem {
    bool unhandled = true;  // false: a rejectionhandled notification
    bool handledSince = false;
    ev::Persistent promise;
    ev::Persistent reason;
};

struct RejectionState {
    RejectionSink sink;
    std::vector<RejectionItem> queue;
    bool flushQueued = false;
};

// Heap-owned and freed explicitly (uninstallRejectionTracking): a
// thread_local holding Persistents would be destroyed at thread exit, in an
// order against the runtime's own thread_locals nobody promises.
thread_local RejectionState* t_state = nullptr;

void rejectionHook(ev::PromiseRejectionOperation op, Value promise, Value reason) {
    RejectionState* st = t_state;
    if (!st) return;
    if (op == ev::PromiseRejectionOperation::Handled) {
        for (RejectionItem& item : st->queue) {
            if (item.unhandled && !item.handledSince &&
                ev::toBits(item.promise.get()) == ev::toBits(promise)) {
                item.handledSince = true;
                return;
            }
        }
    }
    RejectionItem item;
    item.unhandled = op == ev::PromiseRejectionOperation::Unhandled;
    item.promise.set(promise);
    item.reason.set(reason);
    st->queue.push_back(std::move(item));
    if (!st->flushQueued && st->sink.queueFlush) {
        st->flushQueued = true;
        st->sink.queueFlush();
    }
}

// A PromiseRejectionEvent when the realm has the class (js/events.js on the
// main thread), else an Event carrying the two members, else a plain object —
// a worker realm may have only the second.
Value makeRejectionEvent(const char* type, const ev::Persistent& promise,
                         const ev::Persistent& reason, bool cancelable) {
    ev::Persistent init(ev::createObject());
    init.set(ev::setProperty(init.get(), "cancelable", ev::fromBool(cancelable)));
    init.set(ev::setProperty(init.get(), "promise", promise.get()));
    init.set(ev::setProperty(init.get(), "reason", reason.get()));
    ev::Persistent typeP(ev::fromUtf8(type));

    for (const char* ctorName : {"PromiseRejectionEvent", "Event"}) {
        ev::GlobalValue ctor = ev::globalValue(ctorName);
        if (!ctor.found || !ev::isFunction(ctor.value)) continue;
        ev::Persistent ctorP(ctor.value);
        Value args[2] = {typeP.get(), init.get()};
        ev::CallResult made = ev::construct(ctorP.get(), std::span<const Value>(args, 2));
        if (made.thrown || !ev::isObject(made.value)) continue;
        ev::Persistent evt(made.value);
        if (std::string(ctorName) == "Event") {
            evt.set(ev::setProperty(evt.get(), "promise", promise.get()));
            evt.set(ev::setProperty(evt.get(), "reason", reason.get()));
        }
        return evt.get();
    }
    ev::Persistent evt(init.get());
    evt.set(ev::setProperty(evt.get(), "type", typeP.get()));
    evt.set(ev::setProperty(evt.get(), "defaultPrevented", ev::fromBool(false)));
    return evt.get();
}

// `defaultPrevented` as the event reports it after dispatch.
bool eventWasCanceled(const ev::Persistent& evt) {
    Value dp = ev::getProperty(evt.get(), "defaultPrevented");
    return ev::isBool(dp) && ev::toBool(dp);
}

void cancelEvent(const ev::Persistent& evt) {
    ev::Persistent pd(ev::getProperty(evt.get(), "preventDefault"));
    if (ev::isFunction(pd.get())) {
        ev::call(pd.get(), evt.get(), {});
    } else {
        ev::setProperty(evt.get(), "defaultPrevented", ev::fromBool(true));
    }
}

void fireOne(RejectionState& st, const RejectionItem& item) {
    const char* type = item.unhandled ? "unhandledrejection" : "rejectionhandled";
    ev::Persistent evt(makeRejectionEvent(type, item.promise, item.reason, item.unhandled));

    bool canceled = false;
    ev::Persistent global(st.sink.global ? st.sink.global() : ev::undefined());
    if (ev::isObject(global.get())) {
        // The event-handler attribute: `window.onunhandledrejection`.
        const std::string attr = std::string("on") + type;
        ev::Persistent handler(ev::getProperty(global.get(), attr));
        if (ev::isFunction(handler.get())) {
            Value arg = evt.get();
            ev::CallResult r = ev::call(handler.get(), global.get(), std::span<const Value>(&arg, 1));
            if (r.thrown) {
                reportBronzeError(attr.c_str(), r.value);
            } else if (item.unhandled && ev::isBool(r.value) && !ev::toBool(r.value)) {
                cancelEvent(evt);
            }
        }
    }
    if (st.sink.dispatch) st.sink.dispatch(evt.get());
    canceled = item.unhandled && eventWasCanceled(evt);

    if (item.unhandled && !canceled) {
        LOG_ERROR("[bronze:%s] Uncaught (in promise) %s", st.sink.what,
                  thrownValueText(item.reason.get()).c_str());
        if (st.sink.onReported) st.sink.onReported();
    }
}

}  // namespace

void installRejectionTracking(RejectionSink sink) {
    if (!t_state) t_state = new RejectionState();
    t_state->sink = std::move(sink);
    ev::setPromiseRejectionHook(&rejectionHook);
}

void uninstallRejectionTracking() {
    ev::setPromiseRejectionHook(nullptr);
    delete t_state;
    t_state = nullptr;
}

void flushRejectionEvents() {
    RejectionState* st = t_state;
    if (!st) return;
    st->flushQueued = false;
    if (st->queue.empty()) return;
    // The batch is taken whole: a handler that rejects another promise queues
    // it for the NEXT notify step, as the spec's list copy does.
    std::vector<RejectionItem> batch;
    batch.swap(st->queue);
    for (const RejectionItem& item : batch) {
        if (item.unhandled && item.handledSince) continue;
        fireOne(*st, item);
        if (t_state != st) return;  // a handler tore the realm down
    }
}

bool rejectionEventsPending() {
    return t_state && !t_state->queue.empty();
}

void installMainThreadRejectionTracking() {
    RejectionSink sink;
    sink.what = "unhandledrejection";
    sink.queueFlush = [] { postHostTask([] { flushRejectionEvents(); }); };
    sink.global = [] {
        ev::GlobalValue win = ev::globalValue("window");
        return (win.found && ev::isObject(win.value)) ? win.value : ev::undefined();
    };
    sink.dispatch = [](Value evt) { hostDispatchToWindow(evt); };
    sink.onReported = [] {
        setTestFailure(true);
        if (engine::Engine* e = hostEngine()) e->setTestFailure(true);
    };
    installRejectionTracking(std::move(sink));
}

}  // namespace bro::bronze_host
