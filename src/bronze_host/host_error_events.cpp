// The window's `error` event: what an exception nothing caught turns into.
//
// Every uncaught throw out of compiled code a host seam runs — a timer, an
// rAF callback, an event listener, a queueMicrotask callback, a script's top
// level — reaches `reportBronzeError` (dom_globals.cpp) or eval_jit.cpp's
// `reportCallResult`. Both ask this file first, which does what a browser's
// "report the exception" step does:
//
//   1. `window.onerror(message, filename, lineno, colno, error)` — the legacy
//      five-argument form, not an event object. Returning `true` cancels.
//   2. an ErrorEvent (`cancelable`, `message`/`filename`/`lineno`/`colno`/
//      `error`) dispatched at the window, so `addEventListener('error', ...)`
//      fires; `preventDefault()` cancels.
//
// A cancelled error is the page saying it handled it, so the caller skips its
// log line. The old QuickJS runtime did the same from its dispatchHostHook.
//
// bronze records no source position on an Error, so `filename`/`lineno`/
// `colno` are empty and zero; `message` is `Uncaught Name: message`, the text
// a browser gives.
//
// A promise nothing handled is the other report, `unhandledrejection`, raised
// from bronze's rejection hook (host_rejection_events.cpp).

#include "bronze_host/host_internal.h"
#include "util/log.h"

namespace bro::bronze_host {

namespace {

// Re-entrancy guard: an error thrown by an `error` handler is logged, never
// re-dispatched (the web does the same — it would loop forever otherwise).
int g_inErrorDispatch = 0;

struct DispatchGuard {
    DispatchGuard() { ++g_inErrorDispatch; }
    ~DispatchGuard() { --g_inErrorDispatch; }
};

// `Name: message` for an Error, the ToString of anything else. Not
// thrownValueText: that prefers `stack`, which is multi-line and is not what
// ErrorEvent.message carries.
std::string errorMessageText(const ev::Persistent& thrown) {
    Value v = thrown.get();
    if (ev::isObject(v)) {
        Value msgV = ev::getProperty(thrown.get(), "message");
        if (ev::isString(msgV)) {
            std::string msg = ev::toUtf8(msgV);
            Value nameV = ev::getProperty(thrown.get(), "name");
            std::string name = ev::isString(nameV) ? ev::toUtf8(nameV) : std::string("Error");
            if (name.empty()) return msg;
            return msg.empty() ? name : name + ": " + msg;
        }
    }
    return thrownValueText(thrown.get());
}

}  // namespace

bool hostDispatchUncaughtError(Value thrown) {
    if (g_inErrorDispatch > 0) return false;
    if (!hostEngine()) return false;
    DispatchGuard guard;

    ev::Persistent thrownP(thrown);
    const std::string message = "Uncaught " + errorMessageText(thrownP);

    ev::GlobalValue win = ev::globalValue("window");
    if (!win.found || !ev::isObject(win.value)) return false;
    ev::Persistent winP(win.value);

    bool cancelled = false;

    // 1. window.onerror, the five-argument form.
    {
        Value handler = ev::getProperty(winP.get(), "onerror");
        if (ev::isFunction(handler)) {
            ev::Persistent handlerP(handler);
            ev::Persistent msgP(ev::fromUtf8(message));
            ev::Persistent fileP(ev::fromUtf8(""));
            Value args[5] = {msgP.get(), fileP.get(), ev::fromDouble(0), ev::fromDouble(0),
                             thrownP.get()};
            ev::CallResult r = ev::call(handlerP.get(), winP.get(), std::span<const Value>(args, 5));
            if (r.thrown) {
                LOG_ERROR("[bronze:window.onerror] the error handler itself threw: %s",
                          thrownValueText(r.value).c_str());
            } else if (ev::isBool(r.value) && ev::toBool(r.value)) {
                cancelled = true;
            }
        }
    }

    // 2. The ErrorEvent at the window's listeners.
    ev::GlobalValue ctor = ev::globalValue("ErrorEvent");
    if (!ctor.found || !ev::isFunction(ctor.value)) return cancelled;
    ev::Persistent ctorP(ctor.value);

    ev::Persistent init(ev::createObject());
    // Each string in its own statement: an allocating argument next to a
    // slot read of `init` would leave that read stale (embed.h).
    ev::Persistent msgV(ev::fromUtf8(message));
    ev::Persistent fileV(ev::fromUtf8(""));
    init.set(ev::setProperty(init.get(), "cancelable", ev::fromBool(true)));
    init.set(ev::setProperty(init.get(), "message", msgV.get()));
    init.set(ev::setProperty(init.get(), "filename", fileV.get()));
    init.set(ev::setProperty(init.get(), "lineno", ev::fromDouble(0)));
    init.set(ev::setProperty(init.get(), "colno", ev::fromDouble(0)));
    init.set(ev::setProperty(init.get(), "error", thrownP.get()));

    ev::Persistent typeP(ev::fromUtf8("error"));
    Value cargs[2] = {typeP.get(), init.get()};
    ev::CallResult made = ev::construct(ctorP.get(), std::span<const Value>(cargs, 2));
    if (made.thrown || !ev::isObject(made.value)) return cancelled;
    ev::Persistent evtP(made.value);

    Value notPrevented = hostDispatchToWindow(evtP.get());
    if (ev::isBool(notPrevented) && !ev::toBool(notPrevented)) cancelled = true;
    return cancelled;
}

}  // namespace bro::bronze_host
