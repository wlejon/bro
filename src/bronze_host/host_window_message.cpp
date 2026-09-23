// `window.postMessage(message, targetOrigin, transfer)` on the main window:
// the message is structured-cloned at the call (brokit's `structuredClone`,
// the path MessageChannel's ports use) and delivered LATER, as a task — a
// `message` MessageEvent at the window, `onmessage` first and then the
// `addEventListener('message')` listeners.
//
// A task, not a microtask: on the web a posted message is a task on the
// posted-message queue, so code after the call and every promise job it
// queued run before any listener sees the message. postHostTask is this
// layer's task queue, drained at the top of the frame seam.
//
// The page's origin is `bro://app` (location.origin). `targetOrigin` is
// checked the way the spec does: `*` always delivers, `/` means the page's
// own origin, an absolute URL delivers only when its origin matches, and
// anything that is not a URL is a SyntaxError.
//
// A secondary window's `postMessage` is its own (host_window_open.cpp); this
// is the main realm's.

#include "bronze_host/host_internal.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

constexpr const char* kPageOrigin = "bro://app";

// The origin of an absolute URL — scheme://host[:port] — or empty when `url`
// is not one.
std::string originOf(const std::string& url) {
    const size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return {};
    for (size_t i = 0; i < sep; ++i) {
        const char c = url[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
        if (!ok) return {};
    }
    const size_t hostStart = sep + 3;
    size_t hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string::npos) hostEnd = url.size();
    if (hostEnd == hostStart) return {};
    std::string origin = url.substr(0, hostEnd);
    for (size_t i = 0; i < sep; ++i) {
        if (origin[i] >= 'A' && origin[i] <= 'Z') origin[i] = static_cast<char>(origin[i] - 'A' + 'a');
    }
    return origin;
}

// Deliver one cloned message at the window. Runs from the host task queue.
void deliverWindowMessage(const ev::Persistent& data, const ev::Persistent& ports) {
    ev::GlobalValue win = ev::globalValue("window");
    ev::GlobalValue ctor = ev::globalValue("MessageEvent");
    if (!win.found || !ev::isObject(win.value) || !ctor.found || !ev::isFunction(ctor.value)) {
        return;
    }
    ev::Persistent winP(win.value);
    ev::Persistent ctorP(ctor.value);

    ev::Persistent init(ev::createObject());
    ev::Persistent originV(ev::fromUtf8(kPageOrigin));
    init.set(ev::setProperty(init.get(), "data", data.get()));
    init.set(ev::setProperty(init.get(), "origin", originV.get()));
    init.set(ev::setProperty(init.get(), "source", winP.get()));
    init.set(ev::setProperty(init.get(), "ports", ports.get()));

    ev::Persistent typeP(ev::fromUtf8("message"));
    Value cargs[2] = {typeP.get(), init.get()};
    ev::CallResult made = ev::construct(ctorP.get(), std::span<const Value>(cargs, 2));
    if (made.thrown) {
        reportBronzeError("window message", made.value);
        return;
    }
    ev::Persistent evtP(made.value);

    Value onmessage = ev::getProperty(winP.get(), "onmessage");
    if (ev::isFunction(onmessage)) {
        ev::Persistent handler(onmessage);
        Value arg = evtP.get();
        ev::CallResult r = ev::call(handler.get(), winP.get(), std::span<const Value>(&arg, 1));
        if (r.thrown) reportBronzeError("window onmessage", r.value);
    }

    // The listeners, through the engine's window dispatch, handed this same
    // MessageEvent object (host_dom_events.cpp's provided-event path).
    hostDispatchToWindow(evtP.get());
}

Value windowPostMessage(Value, std::span<const Value> a) {
    if (a.empty()) {
        return ev::throwTypeError("Window.postMessage: 1 argument required, but only 0 present");
    }
    ev::Persistent message(a[0]);

    // The two overloads: (message, targetOrigin, transfer) and
    // (message, {targetOrigin, transfer}). The options form defaults the
    // origin to "/".
    std::string targetOrigin = "/";
    ev::Persistent transfer(ev::undefined());
    if (a.size() > 1 && ev::isObject(a[1]) && !ev::isFunction(a[1])) {
        ev::Persistent opts(a[1]);
        Value to = ev::getProperty(opts.get(), "targetOrigin");
        if (!ev::isUndefined(to)) targetOrigin = ev::toUtf8(to);
        transfer.set(ev::getProperty(opts.get(), "transfer"));
    } else if (a.size() > 1) {
        targetOrigin = ev::toUtf8(a[1]);
        if (a.size() > 2) transfer.set(a[2]);
    } else {
        return ev::throwTypeError(
            "Window.postMessage: a targetOrigin is required (use '*' or '/')");
    }

    bool deliver = true;
    if (targetOrigin != "*" && targetOrigin != "/") {
        const std::string origin = originOf(targetOrigin);
        if (origin.empty()) {
            return ev::throwValue(hostMakeDomError(
                "SyntaxError", "Window.postMessage: '" + targetOrigin + "' is not a valid origin"));
        }
        deliver = origin == kPageOrigin;
    }

    // Clone NOW, at the call: the receiver must see the value as it was, and
    // an uncloneable value is the caller's DataCloneError, not a listener's.
    ev::GlobalValue sc = ev::globalValue("structuredClone");
    if (!sc.found || !ev::isFunction(sc.value)) {
        return ev::throwError("Window.postMessage: structuredClone is not installed");
    }
    ev::Persistent scP(sc.value);
    ev::Persistent cloneOpts(ev::undefined());
    std::vector<ev::Persistent> portList;
    if (ev::isObject(transfer.get())) {
        cloneOpts.set(ev::createObject());
        cloneOpts.set(ev::setProperty(cloneOpts.get(), "transfer", transfer.get()));
        // MessagePorts in the transfer list travel as the event's `ports`.
        Value lenV = ev::getProperty(transfer.get(), "length");
        const uint32_t n = ev::isNumber(lenV) ? static_cast<uint32_t>(ev::toDouble(lenV)) : 0u;
        ev::Persistent mpCtor(ev::undefined());
        {
            ev::GlobalValue mp = ev::globalValue("MessagePort");
            if (mp.found) mpCtor.set(mp.value);
        }
        ev::Persistent objectCtor(ev::undefined());
        {
            ev::GlobalValue obj = ev::globalValue("Object");
            if (obj.found) objectCtor.set(obj.value);
        }
        if (ev::isFunction(mpCtor.get()) && ev::isObject(objectCtor.get()) && n > 0) {
            ev::Persistent mpProto(ev::getProperty(mpCtor.get(), "prototype"));
            ev::Persistent getProto(ev::getProperty(objectCtor.get(), "getPrototypeOf"));
            for (uint32_t i = 0; i < n; ++i) {
                ev::Persistent item(ev::getElement(transfer.get(), i));
                if (!ev::isObject(item.get())) continue;
                Value arg = item.get();
                ev::CallResult pr =
                    ev::call(getProto.get(), ev::undefined(), std::span<const Value>(&arg, 1));
                if (pr.thrown || ev::toBits(pr.value) != ev::toBits(mpProto.get())) continue;
                portList.push_back(std::move(item));
            }
        }
    }
    ev::Persistent ports(hostArrayOf(portList.size(),
                                     [&portList](size_t i) { return portList[i].get(); }));
    Value cargs[2] = {message.get(), cloneOpts.get()};
    ev::CallResult cloned = ev::call(scP.get(), ev::undefined(),
                                     std::span<const Value>(cargs, ev::isUndefined(cloneOpts.get()) ? 1 : 2));
    if (cloned.thrown) return ev::throwValue(cloned.value);
    if (!deliver) return ev::undefined();

    ev::Persistent data(cloned.value);
    postHostTask([data, ports]() { deliverWindowMessage(data, ports); });
    return ev::undefined();
}

}  // namespace

Value makeWindowPostMessage() {
    return ev::makeFunction(&windowPostMessage, 2, "postMessage");
}

}  // namespace bro::bronze_host
