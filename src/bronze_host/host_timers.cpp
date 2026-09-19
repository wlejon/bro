// setTimeout / clearTimeout / setInterval / clearInterval for a bronze-compiled
// app, plus the main-thread task queue the other host bindings deliver
// completions through.
// CLOCK AND RESOLUTION.
// This timer table runs on hostClockMs(), the accumulated Engine::onFrame
// deltas (engineNowMs_ scaled by timescale).
//
// WHERE THEY FIRE. fireHostTimers runs once per frame from the bronze frame
// seam, before requestAnimationFrame. Resolution is one frame: timers whose
// deadline has passed fire in creation order.
//
// LIFETIME. A timer's callback lives in an ev::Persistent, which is a GC root.
// A one-shot's entry is erased as it fires, so its root goes with it. An
// interval's does NOT: it is rooted until clearInterval(id) removes it, or the
// process exits. That is the web's behaviour too — an uncleared interval keeps
// its whole closure alive forever — but on the web the tab eventually closes,
// and here the only bound is the process. Nothing reaps them, deliberately:
// a host that dropped a timer the app never cleared would be a silent
// behaviour change, not a leak fix. The warning below is the diagnostic.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/gl_internal.h"  // argAt / numAt / i32At

#include "util/log.h"

#include <algorithm>
#include <deque>
#include <vector>

namespace bro::bronze_host {

namespace {

struct TimerEntry {
    int32_t id = 0;
    // Exactly one of these fires. `native` is host work scheduled by this layer
    // (AbortSignal.timeout's deadline); `fn` is the app's own callback. They
    // share the table so they share the ordering — a host deadline and an app
    // setTimeout set for the same moment fire in creation order, as two tables
    // could not have agreed on.
    std::function<void()> native;
    ev::Persistent fn;
    // setTimeout(fn, delay, a, b) hands a and b to fn. three.js never uses
    // them, but a callback silently called with none is the kind of divergence
    // that surfaces as a wrong render rather than an error.
    std::vector<ev::Persistent> args;
    double dueMs = 0.0;
    double intervalMs = 0.0;
    bool repeating = false;
    // requestIdleCallback: the callback takes an IdleDeadline rather than the
    // app's args, and `timeout` records whether the deadline was the timeout.
    bool idle = false;
    double idleTimeoutMs = 0.0;
    dom::Document* doc = nullptr;
};

// Process-lived and never freed, the same convention HostState follows
// (dom_globals.cpp): these hold ev::Persistents, and a static destructor
// running at process exit would release root slots against a runtime whose own
// statics may already be gone.
std::vector<TimerEntry>* g_timers = nullptr;
std::deque<std::function<void()>>* g_tasks = nullptr;

int32_t g_nextTimerId = 1;
bool g_timerCountWarned = false;

// High enough that no honest app reaches it and low enough to catch a runaway
// while the log is still readable. Warned once: the point is to name the leak,
// not to narrate it.
constexpr size_t kTimerCountWarnAt = 1024;

std::vector<TimerEntry>& timers() {
    if (!g_timers) g_timers = new std::vector<TimerEntry>();
    return *g_timers;
}

std::deque<std::function<void()>>& tasks() {
    if (!g_tasks) g_tasks = new std::deque<std::function<void()>>();
    return *g_tasks;
}

Value addTimer(std::span<const Value> a, bool repeating) {
    Value fn = argAt(a, 0);
    if (!ev::isFunction(fn)) {
        // The web coerces a string first argument to code. bronze has no eval
        // and never will, so this is a named refusal rather than a timer that
        // silently never fires.
        return ev::throwTypeError(
            "setTimeout/setInterval: the first argument must be a function");
    }

    TimerEntry entry;
    entry.id = g_nextTimerId++;
    entry.fn = ev::Persistent(fn);
    for (size_t i = 2; i < a.size(); ++i) entry.args.emplace_back(a[i]);

    // HTML clamps a negative or non-finite delay to 0; NaN takes the same road
    // because every comparison against it is false.
    double delay = numAt(a, 1);
    if (!(delay > 0.0)) delay = 0.0;
    entry.dueMs = hostClockMs() + delay;
    entry.intervalMs = delay;
    entry.repeating = repeating;
    entry.doc = currentHostDocument();

    auto& list = timers();
    list.push_back(std::move(entry));
    if (!g_timerCountWarned && list.size() >= kTimerCountWarnAt) {
        g_timerCountWarned = true;
        LOG_WARN("bronze_host: %zu live timers. Every one roots its callback and "
                 "everything the callback closes over until clearTimeout/"
                 "clearInterval removes it; nothing else reaps them.",
                 list.size());
    }
    return ev::fromDouble(static_cast<double>(list.back().id));
}

// requestIdleCallback(fn, {timeout}) — an idle period here is the next frame:
// the engine has no notion of a busy main thread between frames (there is
// no other task source), so the callback runs on the next tick with a
// 50 ms budget, which is the spec's cap on an idle period. `didTimeout` is
// true only when the app gave a timeout and the frame arrived after it.
Value addIdleCallback(std::span<const Value> a) {
    Value fn = argAt(a, 0);
    if (!ev::isFunction(fn)) {
        return ev::throwTypeError("requestIdleCallback: the first argument must be a function");
    }
    TimerEntry entry;
    entry.id = g_nextTimerId++;
    entry.fn = ev::Persistent(fn);
    entry.dueMs = hostClockMs();
    entry.idle = true;
    Value opts = argAt(a, 1);
    if (ev::isObject(opts)) {
        Value t = ev::getProperty(opts, "timeout");
        if (ev::isNumber(t) && ev::toDouble(t) > 0.0) entry.idleTimeoutMs = ev::toDouble(t);
    }
    entry.doc = currentHostDocument();
    timers().push_back(std::move(entry));
    return ev::fromDouble(static_cast<double>(timers().back().id));
}

Value makeIdleDeadline(bool didTimeout) {
    const double start = hostClockMs();
    ObjectBuilder b;
    b.set("didTimeout", ev::fromBool(didTimeout));
    b.def("timeRemaining", 0, [start](Value, std::span<const Value>) {
        double left = 50.0 - (hostClockMs() - start);
        return ev::fromDouble(left > 0.0 ? left : 0.0);
    });
    return b.get();
}

Value clearTimer(std::span<const Value> a) {
    const int32_t id = i32At(a, 0);
    auto& list = timers();
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->id == id) {
            list.erase(it);
            break;
        }
    }
    return ev::undefined();
}

}  // namespace

// ---------------------------------------------------------------------------
// The main-thread task queue
// ---------------------------------------------------------------------------

void postHostTask(std::function<void()> task) {
    tasks().push_back(std::move(task));
}

int32_t hostSetTimeout(std::function<void()> task, double delayMs) {
    if (!(delayMs > 0.0)) delayMs = 0.0;  // NaN and negatives clamp, as HTML's do
    TimerEntry entry;
    entry.id = g_nextTimerId++;
    entry.native = std::move(task);
    entry.dueMs = hostClockMs() + delayMs;
    entry.intervalMs = delayMs;
    entry.repeating = false;
    timers().push_back(std::move(entry));
    return timers().back().id;
}

void drainHostTasks() {
    auto& queue = tasks();
    if (queue.empty()) return;
    // Move the batch out before running any of it, so a task that posts another
    // task — an onload handler that starts the next load — queues it for the
    // NEXT frame and one frame cannot starve on a self-feeding chain. Same rule
    // the rAF queue follows (dom_globals.cpp), for the same reason.
    std::deque<std::function<void()>> batch;
    batch.swap(queue);
    for (auto& task : batch) task();
}

// ---------------------------------------------------------------------------
// Firing
// ---------------------------------------------------------------------------

void fireHostTimers(double nowMs) {
    auto& list = timers();
    if (list.empty()) return;

    // Decide the batch BEFORE running any of it. A callback that adds a timer
    // must not have it fire in the same tick (that is how a setTimeout(0) loop
    // becomes an infinite one), and a callback that clears a sibling must be
    // obeyed — which the re-find by id below is what honours.
    struct Due {
        double dueMs;
        int32_t id;
    };
    std::vector<Due> due;
    for (const TimerEntry& entry : list) {
        if (entry.dueMs <= nowMs) due.push_back({entry.dueMs, entry.id});
    }
    if (due.empty()) return;
    // HTML fires same-deadline timers in creation order, and ids are handed out
    // in creation order.
    std::sort(due.begin(), due.end(), [](const Due& x, const Due& y) {
        return x.dueMs != y.dueMs ? x.dueMs < y.dueMs : x.id < y.id;
    });

    for (const Due& d : due) {
        auto it = std::find_if(list.begin(), list.end(),
                               [&](const TimerEntry& e) { return e.id == d.id; });
        if (it == list.end()) continue;  // cleared by an earlier callback

        // Take what the call needs, then settle the table BEFORE calling. A
        // one-shot is already gone, so clearTimeout from inside its own
        // callback is the no-op it is on the web; a repeat already carries its
        // next deadline, so clearInterval from inside removes a live entry and
        // actually stops it. Settling afterwards would resurrect a timer the
        // callback had just cleared.
        std::function<void()> native = it->native;
        ev::Persistent fn = it->fn;
        std::vector<ev::Persistent> args = it->args;
        dom::Document* entryDoc = it->doc;
        if (it->idle) {
            const bool didTimeout = it->idleTimeoutMs > 0.0 &&
                                    nowMs - it->dueMs >= it->idleTimeoutMs;
            args.clear();
            args.emplace_back(makeIdleDeadline(didTimeout));
        }
        if (it->repeating) {
            // Advance from the DEADLINE, so a long frame does not stretch the
            // interval — but skip whole missed periods instead of firing a
            // catch-up burst: a 16 ms interval across a 500 ms stall is thirty
            // calls nobody asked for, and the engine's own clock already
            // freezes rather than accumulating while paused.
            double next = it->dueMs + it->intervalMs;
            if (next <= nowMs) next = nowMs + it->intervalMs;
            it->dueMs = next;
        } else {
            list.erase(it);
        }
        // `it` is dead from here on — the call below can push onto `list`
        // (another setTimeout) and reallocate it. Nothing after this point
        // touches the iterator; the next round re-finds by id.

        if (native) {
            native();
            continue;
        }

        dom::Document* prevDoc = currentHostDocument();
        ev::GlobalValue docG = ev::globalValue("document");
        // Held in a Persistent, not a raw Value: the callback below may
        // allocate enough to move the heap, and the restore after it must
        // name the document's CURRENT address. `globalThis` is re-read after
        // the call for the same reason.
        ev::Persistent prevDocVal(docG.found ? docG.value : ev::null());
        if (entryDoc) {
            enterRealmScope(scopeIdForDocument(entryDoc));
            setCurrentHostDocument(entryDoc);
            ev::Persistent subDocVal(hostDocumentValue(entryDoc));
            ev::registerGlobal("document", subDocVal.get());
            ev::GlobalValue gt = ev::globalValue("globalThis");
            if (gt.found && ev::isObject(gt.value)) {
                ev::setProperty(gt.value, "document", subDocVal.get());
            }
        }

        std::vector<Value> argv;
        argv.reserve(args.size());
        for (ev::Persistent& arg : args) argv.push_back(arg.get());
        // Nothing has allocated since the argv reads, so every Value in it is
        // current when `call` roots them.
        ev::CallResult r = ev::call(fn.get(), ev::undefined(),
                                    std::span<const Value>(argv.data(), argv.size()));
        if (r.thrown) reportBronzeError("timer", r.value);

        if (entryDoc) {
            if (!ev::isNull(prevDocVal.get())) {
                ev::registerGlobal("document", prevDocVal.get());
                ev::GlobalValue gt = ev::globalValue("globalThis");
                if (gt.found && ev::isObject(gt.value)) {
                    ev::setProperty(gt.value, "document", prevDocVal.get());
                }
            }
            setCurrentHostDocument(prevDoc);
            exitRealmScope();
        }
    }
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

void installTimerGlobals() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    bool hasGt = gt.found && ev::isObject(gt.value);
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return addTimer(a, /*repeating=*/false); },
            2);
        ev::registerGlobal("setTimeout", fn);
        if (hasGt) ev::setProperty(gt.value, "setTimeout", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return clearTimer(a); }, 1);
        ev::registerGlobal("clearTimeout", fn);
        if (hasGt) ev::setProperty(gt.value, "clearTimeout", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return addTimer(a, /*repeating=*/true); },
            2);
        ev::registerGlobal("setInterval", fn);
        if (hasGt) ev::setProperty(gt.value, "setInterval", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return clearTimer(a); }, 1);
        ev::registerGlobal("clearInterval", fn);
        if (hasGt) ev::setProperty(gt.value, "clearInterval", fn);
    }
    {
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return addIdleCallback(a); }, 2);
        ev::registerGlobal("requestIdleCallback", fn);
        if (hasGt) ev::setProperty(gt.value, "requestIdleCallback", fn);
    }
    {
        // Same table, same ids: cancelIdleCallback(id) is clearTimeout(id).
        Value fn = ev::makeFunction(
            [](Value, std::span<const Value> a) { return clearTimer(a); }, 1);
        ev::registerGlobal("cancelIdleCallback", fn);
        if (hasGt) ev::setProperty(gt.value, "cancelIdleCallback", fn);
    }
}

void clearHostTimers() {
    if (g_timers) g_timers->clear();
    if (g_tasks) g_tasks->clear();
}

void clearHostTimersForDocument(dom::Document* doc) {
    if (!g_timers || !doc) return;
    auto it = std::remove_if(g_timers->begin(), g_timers->end(),
                             [doc](const TimerEntry& t) { return t.doc == doc; });
    g_timers->erase(it, g_timers->end());
}

}  // namespace bro::bronze_host
