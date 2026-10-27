// `window.postMessage(message, targetOrigin, transfer)`, and the delivery of a
// `message` MessageEvent at ANY window of the process — the main app window, a
// secondary bro window (bro.window.open / window.open), an <iframe>, an
// <appview>.
//
// Every window is a compartment of its own (host_compartment.h): its own
// realm, its own `window`. A post to the caller's own window is cloned with
// brokit's `structuredClone` (the path MessageChannel's ports use), in its
// realm. A post to ANOTHER window crosses compartments, so nothing of the
// sender's heap may reach the receiver: the message is serialized to bytes at
// the call (host_worker_msg.h, the Worker path) and read back inside the
// receiver's realm, so every object the receiver sees is its own.
//
// Either way the message is delivered LATER, as a task: a `message`
// MessageEvent at the window, `onmessage` first and then the
// `addEventListener('message')` listeners. On the web a posted message is a
// task on the posted-message queue, so code after the call and every promise
// job it queued run before any listener sees it. postHostTask is this layer's
// task queue, drained at the top of the frame seam.
//
// The page's origin is `bro://app` (location.origin), in every realm.
// `targetOrigin` is checked the way the spec does: `*` always delivers, `/`
// means the page's own origin, an absolute URL delivers only when its origin
// matches, and anything that is not a URL is a SyntaxError.

#include "bronze_host/host_window_open.h"
#include "bronze_host/host_compartment.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_window_message.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_document.h"
#include "engine/engine.h"
#include "util/log.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

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

bool isArrayLike(Value v) {
    if (!ev::isObject(v) || ev::isFunction(v)) return false;
    return ev::isNumber(ev::getProperty(v, "length"));
}

// The document a delivery recorded at the post is still live: the app's
// always, a sub-document while its compartment is.
bool documentLive(dom::Document* doc) {
    if (!doc) return true;
    Compartment* c = compartmentForDocument(doc);
    return c && c->kind != CompartmentKind::App && c->document == doc && !c->dying;
}

// The MessageEvent itself, and its dispatch, in the receiver's compartment
// (entered by the caller).
void dispatchMessageEvent(dom::Document* target, const ev::Persistent& data, const ev::Persistent& ports,
                          const ev::Persistent& source, const std::string& origin) {
    ev::GlobalValue ctor = ev::globalValue("MessageEvent");
    if (!ctor.found || !ev::isFunction(ctor.value)) return;
    ev::Persistent ctorP(ctor.value);

    ev::GlobalValue win = ev::globalValue("window");
    if (!win.found || !ev::isObject(win.value)) return;
    ev::Persistent winP(win.value);

    ev::Persistent init(ev::createObject());
    ev::Persistent originV(ev::fromUtf8(origin));
    ev::Persistent portsV(ports.get());
    if (!ev::isObject(portsV.get())) portsV.set(hostArrayOf(0, [](size_t) { return ev::undefined(); }));
    init.set(ev::setProperty(init.get(), "data", data.get()));
    init.set(ev::setProperty(init.get(), "origin", originV.get()));
    init.set(ev::setProperty(init.get(), "source", source.get()));
    init.set(ev::setProperty(init.get(), "ports", portsV.get()));

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

    // The listeners registered on THIS document's window, handed this same
    // MessageEvent object (host_dom_events.cpp's provided-event path).
    hostDispatchToWindowOf(target ? target : (hostEngine() ? hostEngine()->document() : nullptr), evtP.get());
}

Value windowPostMessage(Value, std::span<const Value> a) {
    if (a.empty()) {
        return ev::throwTypeError("Window.postMessage: 1 argument required, but only 0 present");
    }
    ev::Persistent message(a[0]);

    PostMessageTarget target;
    parsePostMessageArgs(a, "Window.postMessage", /*legacyTransferArray=*/false, target);

    // Clone NOW, at the call: the receiver must see the value as it was, and
    // an uncloneable value is the caller's DataCloneError, not a listener's.
    ev::Persistent data(ev::undefined());
    ev::Persistent ports(ev::undefined());
    cloneForPostMessage(message, target.transfer, data, ports);
    if (!target.deliver) return ev::undefined();

    // The window that posted is the window that receives: its document,
    // recorded now (null for the app's). `source` is that window.
    Compartment* self = currentCompartment();
    dom::Document* doc = self->kind == CompartmentKind::App ? nullptr : self->document;
    ev::GlobalValue win = ev::globalValue("window");
    auto source = std::make_shared<ev::Persistent>(win.found ? win.value : ev::null());
    postHostTask([doc, data, ports, source]() {
        deliverWindowMessageEvent(doc, data, ports, [source] { return source->get(); }, kHostPageOrigin);
    });
    return ev::undefined();
}

}  // namespace

void parsePostMessageArgs(std::span<const Value> a, const char* what, bool legacyTransferArray,
                          PostMessageTarget& out) {
    out.targetOrigin = "/";
    out.transfer.set(ev::undefined());
    out.deliver = true;
    // The overloads: (message, targetOrigin, transfer), (message, {targetOrigin,
    // transfer}) and, for a bro.window handle, (message, transfer) — the
    // Worker-style form its first callers were written against.
    if (a.size() > 1 && legacyTransferArray && isArrayLike(a[1])) {
        out.transfer.set(a[1]);
    } else if (a.size() > 1 && ev::isObject(a[1]) && !ev::isFunction(a[1])) {
        ev::Persistent opts(a[1]);
        Value to = ev::getProperty(opts.get(), "targetOrigin");
        if (!ev::isUndefined(to)) out.targetOrigin = ev::toUtf8(to);
        out.transfer.set(ev::getProperty(opts.get(), "transfer"));
    } else if (a.size() > 1 && !ev::isUndefined(a[1])) {
        out.targetOrigin = ev::toUtf8(a[1]);
        if (a.size() > 2) out.transfer.set(a[2]);
    }

    if (out.targetOrigin != "*" && out.targetOrigin != "/") {
        const std::string origin = originOf(out.targetOrigin);
        if (origin.empty()) {
            ev::throwValue(hostMakeDomError(
                "SyntaxError",
                std::string(what) + ": '" + out.targetOrigin + "' is not a valid origin"));
        }
        out.deliver = origin == kHostPageOrigin;
    }
}

void cloneForPostMessage(const ev::Persistent& message, const ev::Persistent& transfer,
                         ev::Persistent& dataOut, ev::Persistent& portsOut) {
    ev::GlobalValue sc = ev::globalValue("structuredClone");
    if (!sc.found || !ev::isFunction(sc.value)) {
        ev::throwError("postMessage: structuredClone is not installed");
    }
    ev::Persistent scP(sc.value);
    ev::Persistent cloneOpts(ev::undefined());
    std::vector<ev::Persistent> portList;
    if (ev::isObject(transfer.get())) {
        cloneOpts.set(ev::createObject());
        cloneOpts.set(ev::setProperty(cloneOpts.get(), "transfer", transfer.get()));
        // MessagePorts in the transfer list travel as the event's `ports`.
        Value lenV = ev::getProperty(transfer.get(), "length");
        const uint32_t n = ev::isNumber(lenV) ? satCast<uint32_t>(ev::toDouble(lenV)) : 0u;
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
    portsOut.set(hostArrayOf(portList.size(), [&portList](size_t i) { return portList[i].get(); }));
    Value cargs[2] = {message.get(), cloneOpts.get()};
    ev::CallResult cloned = ev::call(
        scP.get(), ev::undefined(),
        std::span<const Value>(cargs, ev::isUndefined(cloneOpts.get()) ? 1 : 2));
    if (cloned.thrown) ev::throwValue(cloned.value);
    dataOut.set(cloned.value);
}

void serializeForPost(const ev::Persistent& message, const ev::Persistent& transfer, Message& out) {
    std::vector<ev::Persistent> transfers;
    if (ev::isObject(transfer.get())) {
        Value tv = transfer.get();
        transfers = collectTransferList(std::span<const Value>(&tv, 1), 0);
    }
    const std::vector<Value> transferVals = currentValues(transfers);
    if (!serializeMessage(message.get(), transferVals, out)) {
        ev::throwValue(hostMakeDomError("DataCloneError", "postMessage: the message could not be cloned"));
    }
}

void deliverWindowMessageEvent(dom::Document* doc, const ev::Persistent& data, const ev::Persistent& ports,
                               const std::function<Value()>& makeSource, const std::string& origin) {
    if (!documentLive(doc)) return;  // the window closed before its message arrived
    // An entry of the receiver's: it may run after an earlier one suspended
    // in it, so the window is asked again then.
    runEntry(doc, EntryKind::Async, [doc, data, ports, makeSource, origin] {
        if (!documentLive(doc)) return;
        ev::Persistent source(makeSource ? makeSource() : ev::null());
        dispatchMessageEvent(doc, data, ports, source, origin);
    });
}

void deliverWindowMessage(dom::Document* doc, std::shared_ptr<const Message> msg,
                          const std::function<Value()>& makeSource, const std::string& origin) {
    if (!documentLive(doc) || !msg) return;
    runEntry(doc, EntryKind::Async, [doc, msg, makeSource, origin] {
        if (!documentLive(doc)) return;
        // Read back here, in the receiver's realm: its objects are the
        // receiver's.
        ev::CallResult read = ev::catchThrow([&] { return deserializeMessage(*msg); });
        if (read.thrown) {
            LOG_WARN("%swindow: dropping a message that does not deserialize: %s", compartmentLogPrefix().c_str(),
                     thrownValueText(read.value).c_str());
            return;
        }
        ev::Persistent data(read.value);
        ev::Persistent ports(ev::undefined());
        ev::Persistent source(makeSource ? makeSource() : ev::null());
        dispatchMessageEvent(doc, data, ports, source, origin);
    });
}

Value makeWindowPostMessage() {
    return ev::makeFunction(&windowPostMessage, 2, "postMessage");
}

}  // namespace bro::bronze_host
