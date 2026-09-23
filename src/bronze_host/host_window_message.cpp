// `window.postMessage(message, targetOrigin, transfer)`, and the delivery of a
// `message` MessageEvent at ANY window of the process — the main app window, a
// secondary bro window (bro.window.open / window.open), an <iframe> realm.
//
// The message is structured-cloned at the call (brokit's `structuredClone`,
// the path MessageChannel's ports use) and delivered LATER, as a task: a
// `message` MessageEvent at the window, `onmessage` first and then the
// `addEventListener('message')` listeners.
//
// A task, not a microtask: on the web a posted message is a task on the
// posted-message queue, so code after the call and every promise job it
// queued run before any listener sees the message. postHostTask is this
// layer's task queue, drained at the top of the frame seam.
//
// WHICH window: the one whose realm made the call. Every realm shares one
// heap and one `window` object, with a realm scope swapped in around each
// call into it (host_realm_scope.cpp), so the target is recorded at the call
// — the realm scope and its document — and delivery switches back into that
// realm before it reads `onmessage` or dispatches at that document's window
// listeners. Without that, a secondary window's self-post reached the main
// window. Cross-window posts (a window.open handle and the opened window's
// `window.opener`) are host_window_open.cpp's; they deliver through
// deliverWindowMessageEvent below with the other window as `source`.
//
// The page's origin is `bro://app` (location.origin), in every realm.
// `targetOrigin` is checked the way the spec does: `*` always delivers, `/`
// means the page's own origin, an absolute URL delivers only when its origin
// matches, and anything that is not a URL is a SyntaxError.

#include "bronze_host/host_window_open.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "engine/window_host.h"

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

// Resolve `scopeId`/`doc` as recorded at a post to the document to deliver
// at, or nullptr when that window is gone.
dom::Document* liveDocumentFor(uint64_t scopeId, dom::Document* doc) {
    engine::Engine* eng = hostEngine();
    if (!eng) return nullptr;
    if (scopeId == 0) return eng->document();
    if (engine::WindowHost* wh = eng->windowHostById(scopeId)) {
        if (wh->pendingClose || !wh->document) return nullptr;
        return wh->document.get();
    }
    // An <iframe> realm: its scope id is its document, which is live exactly
    // while the engine still hosts it.
    if (doc && eng->iframeForDocument(doc)) return doc;
    return nullptr;
}

// Enter a realm and make `doc` its current document for the length of a
// delivery: what the frame seam does around a timer or an rAF of that realm.
class RealmEntry {
public:
    RealmEntry(uint64_t scopeId, dom::Document* doc) {
        prevHostDoc_ = currentHostDocument();
        enterRealmScope(scopeId);
        if (doc == prevHostDoc_) return;
        swapped_ = true;
        ev::GlobalValue docG = ev::globalValue("document");
        prevDocVal_.set(docG.found ? docG.value : ev::null());
        setCurrentHostDocument(doc);
        setDocumentGlobal(hostDocumentValue(doc));
    }
    ~RealmEntry() {
        if (swapped_) {
            if (!ev::isNull(prevDocVal_.get())) setDocumentGlobal(prevDocVal_.get());
            setCurrentHostDocument(prevHostDoc_);
        }
        exitRealmScope();
    }
    RealmEntry(const RealmEntry&) = delete;
    RealmEntry& operator=(const RealmEntry&) = delete;

private:
    static void setDocumentGlobal(Value v) {
        ev::Persistent val(v);
        ev::registerGlobal("document", val.get());
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) ev::setProperty(gt.value, "document", val.get());
    }
    dom::Document* prevHostDoc_ = nullptr;
    ev::Persistent prevDocVal_;
    bool swapped_ = false;
};

Value windowPostMessage(Value, std::span<const Value> a) {
    if (a.empty()) {
        return ev::throwTypeError("Window.postMessage: 1 argument required, but only 0 present");
    }
    ev::Persistent message(a[0]);

    PostMessageTarget target;
    Value thrown = ev::undefined();
    if (!parsePostMessageArgs(a, "Window.postMessage", /*legacyTransferArray=*/false, target,
                              thrown)) {
        return thrown;
    }

    // Clone NOW, at the call: the receiver must see the value as it was, and
    // an uncloneable value is the caller's DataCloneError, not a listener's.
    ev::Persistent data(ev::undefined());
    ev::Persistent ports(ev::undefined());
    if (!cloneForPostMessage(message, target.transfer, data, ports, thrown)) return thrown;
    if (!target.deliver) return ev::undefined();

    // The window that posted is the window that receives: its realm and its
    // document, recorded now. `source` is that window.
    dom::Document* doc = currentHostDocument();
    const uint64_t scopeId = scopeIdForDocument(doc);
    ev::GlobalValue win = ev::globalValue("window");
    ev::Persistent source(win.found ? win.value : ev::null());
    postHostTask([scopeId, doc, data, ports, source]() {
        deliverWindowMessageEvent(scopeId, doc, data, ports, source, kHostPageOrigin);
    });
    return ev::undefined();
}

}  // namespace

bool parsePostMessageArgs(std::span<const Value> a, const char* what, bool legacyTransferArray,
                          PostMessageTarget& out, Value& thrown) {
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
            thrown = ev::throwValue(hostMakeDomError(
                "SyntaxError",
                std::string(what) + ": '" + out.targetOrigin + "' is not a valid origin"));
            return false;
        }
        out.deliver = origin == kHostPageOrigin;
    }
    return true;
}

bool cloneForPostMessage(const ev::Persistent& message, const ev::Persistent& transfer,
                         ev::Persistent& dataOut, ev::Persistent& portsOut, Value& thrown) {
    ev::GlobalValue sc = ev::globalValue("structuredClone");
    if (!sc.found || !ev::isFunction(sc.value)) {
        thrown = ev::throwError("postMessage: structuredClone is not installed");
        return false;
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
    portsOut.set(hostArrayOf(portList.size(), [&portList](size_t i) { return portList[i].get(); }));
    Value cargs[2] = {message.get(), cloneOpts.get()};
    ev::CallResult cloned = ev::call(
        scP.get(), ev::undefined(),
        std::span<const Value>(cargs, ev::isUndefined(cloneOpts.get()) ? 1 : 2));
    if (cloned.thrown) {
        thrown = ev::throwValue(cloned.value);
        return false;
    }
    dataOut.set(cloned.value);
    return true;
}

void deliverWindowMessageEvent(uint64_t scopeId, dom::Document* doc, const ev::Persistent& data,
                               const ev::Persistent& ports, const ev::Persistent& source,
                               const std::string& origin) {
    dom::Document* target = liveDocumentFor(scopeId, doc);
    if (!target) return;  // the window closed before its message arrived
    ev::GlobalValue ctor = ev::globalValue("MessageEvent");
    if (!ctor.found || !ev::isFunction(ctor.value)) return;
    ev::Persistent ctorP(ctor.value);

    RealmEntry realm(scopeId, target);

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

    // `onmessage` as THIS realm sees it: the realm scope just entered put its
    // own expandos back on the shared global.
    Value onmessage = ev::getProperty(winP.get(), "onmessage");
    if (ev::isFunction(onmessage)) {
        ev::Persistent handler(onmessage);
        Value arg = evtP.get();
        ev::CallResult r = ev::call(handler.get(), winP.get(), std::span<const Value>(&arg, 1));
        if (r.thrown) reportBronzeError("window onmessage", r.value);
    }

    // The listeners registered on THIS document's window, handed this same
    // MessageEvent object (host_dom_events.cpp's provided-event path).
    hostDispatchToWindowOf(target, evtP.get());
}

Value makeWindowPostMessage() {
    return ev::makeFunction(&windowPostMessage, 2, "postMessage");
}

}  // namespace bro::bronze_host
