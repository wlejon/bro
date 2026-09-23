#include "bronze_host/host_window_open.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_worker_msg.h"
#include "bronze_host/host_realm_scope.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>

#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace {

struct WindowHandleState {
    engine::Engine* engine = nullptr;
    uint64_t id = 0;
    int width = 800;
    int height = 600;
    int x = 0;
    int y = 0;
    std::string title = "bro";
    bool closed = false;
    std::vector<ev::Persistent> closeListeners;
    std::vector<ev::Persistent> loadListeners;
    std::vector<ev::Persistent> resizeListeners;
    std::vector<ev::Persistent> messageListeners;
};

struct ParentMessage {
    uint64_t hostId = 0;
    Message msg;
};

// A window.postMessage between two windows, cloned at the call and waiting for
// the drain: `hostId` is the secondary window on the other end.
struct WindowMessage {
    uint64_t hostId = 0;
    ev::Persistent data;
    ev::Persistent ports;
};

static std::unordered_map<uint64_t, std::shared_ptr<WindowHandleState>> s_handleStates;
static std::unordered_map<uint64_t, ev::Persistent> s_windowHandles;
// handle.postMessage → the opened window's own window (MessageEvent there).
static std::unordered_map<uint64_t, std::vector<WindowMessage>> s_childInboxes;
// bro.window.parent.postMessage → the handle's 'message' listeners.
static std::vector<ParentMessage> s_parentInbox;
// window.opener.postMessage → the main window (MessageEvent there).
static std::vector<WindowMessage> s_openerInbox;
// Each secondary window's `window.opener`: the WindowProxy-shaped object that
// stands for the main window inside it, and the `source` of every message
// the main window posts to it.
static std::unordered_map<uint64_t, ev::Persistent> s_openerProxies;

// The secondary window whose realm is running, or 0.
static uint64_t currentWindowHostId() {
    engine::Engine* eng = hostEngine();
    dom::Document* doc = currentHostDocument();
    if (!eng || !doc) return 0;
    engine::WindowHost* wh = eng->windowHostForDocument(doc);
    return wh ? wh->id : 0;
}

static Value openerProxyFor(uint64_t hostId) {
    auto it = s_openerProxies.find(hostId);
    if (it != s_openerProxies.end()) return it->second.get();

    ObjectBuilder b;
    b.def("postMessage", 2, [hostId](Value, std::span<const Value> a) -> Value {
        if (a.empty()) {
            return ev::throwTypeError(
                "Window.postMessage: 1 argument required, but only 0 present");
        }
        ev::Persistent message(a[0]);
        PostMessageTarget target;
        Value thrown = ev::undefined();
        if (!parsePostMessageArgs(a, "Window.postMessage", false, target, thrown)) return thrown;
        WindowMessage m;
        m.hostId = hostId;
        if (!cloneForPostMessage(message, target.transfer, m.data, m.ports, thrown)) {
            return thrown;
        }
        if (target.deliver) s_openerInbox.push_back(std::move(m));
        return ev::undefined();
    });
    // The main window outlives every secondary one.
    b.accessor("closed", [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(false);
    }, nullptr);
    b.def("focus", 0, [](Value, std::span<const Value>) -> Value {
        return ev::undefined();
    });
    Value proxy = b.get();
    auto [ins, _] = s_openerProxies.emplace(hostId, ev::Persistent(proxy));
    return ins->second.get();
}

static Value makeWindowHandle(std::shared_ptr<WindowHandleState> state) {
    uint64_t id = state->id;
    ObjectBuilder b;
    b.set("id", ev::fromDouble(static_cast<double>(id)));

    b.accessor("closed", [id](Value, std::span<const Value>) -> Value {
        auto it = s_handleStates.find(id);
        if (it == s_handleStates.end() || it->second->closed) return ev::fromBool(true);
        auto* eng = it->second->engine;
        if (!eng) return ev::fromBool(true);
        auto* host = eng->windowHostById(id);
        if (!host) return ev::fromBool(true);
        return ev::fromBool(false);
    }, nullptr);

    b.def("getSize", 0, [id](Value, std::span<const Value>) -> Value {
        int w = 800, h = 600;
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            w = it->second->width;
            h = it->second->height;
            if (it->second->engine) {
                if (auto* host = it->second->engine->windowHostById(id)) {
                    w = host->width;
                    h = host->height;
                }
            }
        }
        ObjectBuilder sz;
        sz.set("width", ev::fromDouble(w));
        sz.set("height", ev::fromDouble(h));
        return sz.get();
    });

    b.def("setSize", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::undefined();
        int w = static_cast<int>(ev::toDouble(a[0]));
        int h = static_cast<int>(ev::toDouble(a[1]));
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            it->second->width = w;
            it->second->height = h;
            if (it->second->engine) {
                if (auto* host = it->second->engine->windowHostById(id)) {
                    host->opts.width = w;
                    host->opts.height = h;
                    host->width = w;
                    host->height = h;
                    if (host->window) host->window->setSize(w, h);
                }
            }
        }
        return ev::undefined();
    });

    b.def("getPosition", 0, [id](Value, std::span<const Value>) -> Value {
        int x = 0, y = 0;
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            x = it->second->x;
            y = it->second->y;
            if (it->second->engine) {
                if (auto* host = it->second->engine->windowHostById(id)) {
                    if (host->window) host->window->getPosition(x, y);
                }
            }
        }
        ObjectBuilder pos;
        pos.set("x", ev::fromDouble(x));
        pos.set("y", ev::fromDouble(y));
        return pos.get();
    });

    b.def("setPosition", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::undefined();
        int x = static_cast<int>(ev::toDouble(a[0]));
        int y = static_cast<int>(ev::toDouble(a[1]));
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            it->second->x = x;
            it->second->y = y;
            if (it->second->engine) {
                if (auto* host = it->second->engine->windowHostById(id)) {
                    if (host->window && !host->opts.hidden) {
                        host->window->setPosition(x, y);
                    }
                }
            }
        }
        return ev::undefined();
    });

    b.def("setTitle", 1, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        std::string t = ev::toUtf8(a[0]);
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            it->second->title = t;
            if (it->second->engine) {
                if (auto* host = it->second->engine->windowHostById(id)) {
                    host->opts.title = t;
                    if (host->window) host->window->setTitle(t);
                }
            }
        }
        return ev::undefined();
    });

    b.def("focus", 0, [id](Value, std::span<const Value>) -> Value {
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end() && it->second->engine) {
            if (auto* host = it->second->engine->windowHostById(id)) {
                if (host->window) host->window->raise();
            }
        }
        return ev::undefined();
    });

    b.def("close", 0, [id](Value, std::span<const Value>) -> Value {
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            if (it->second->closed) return ev::undefined();
            if (it->second->engine) {
                it->second->engine->closeWindowHost(id);
            }
        }
        return ev::undefined();
    });

    b.def("capture", 0, [id](Value, std::span<const Value>) -> Value {
        auto it = s_handleStates.find(id);
        if (it == s_handleStates.end() || !it->second->engine) return ev::null();
        int w = 0, h = 0;
        auto pixels = it->second->engine->captureWindowHost(id, w, h);
        if (pixels.empty() || w <= 0 || h <= 0) return ev::null();
        return makeImageDataValue(w, h, pixels.data());
    });

    b.def("addEventListener", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2 || !ev::isFunction(a[1])) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            if (type == "close") it->second->closeListeners.emplace_back(a[1]);
            else if (type == "load") it->second->loadListeners.emplace_back(a[1]);
            else if (type == "resize") it->second->resizeListeners.emplace_back(a[1]);
            else if (type == "message") it->second->messageListeners.emplace_back(a[1]);
        }
        return ev::undefined();
    });

    b.def("removeEventListener", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::undefined();
        std::string type = ev::toUtf8(a[0]);
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end()) {
            auto removeFn = [&](std::vector<ev::Persistent>& list) {
                for (auto lit = list.begin(); lit != list.end(); ++lit) {
                    if (lit->get() == a[1]) {
                        list.erase(lit);
                        break;
                    }
                }
            };
            if (type == "close") removeFn(it->second->closeListeners);
            else if (type == "load") removeFn(it->second->loadListeners);
            else if (type == "resize") removeFn(it->second->resizeListeners);
            else if (type == "message") removeFn(it->second->messageListeners);
        }
        return ev::undefined();
    });

    // The opened window's postMessage, seen from its opener: a `message`
    // MessageEvent at THAT window (onmessage + its window listeners), with
    // `source` its `window.opener` and origin the page's. Takes the Window
    // overloads plus (message, transferArray), the Worker-style form bro's
    // first callers used. The clone — and so any transfer's detach — happens
    // at the call even when the window has closed.
    b.def("postMessage", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty()) {
            return ev::throwTypeError(
                "Window.postMessage: 1 argument required, but only 0 present");
        }
        ev::Persistent message(a[0]);
        PostMessageTarget target;
        Value thrown = ev::undefined();
        if (!parsePostMessageArgs(a, "Window.postMessage", true, target, thrown)) return thrown;
        WindowMessage m;
        m.hostId = id;
        if (!cloneForPostMessage(message, target.transfer, m.data, m.ports, thrown)) {
            return thrown;
        }
        if (!target.deliver) return ev::undefined();
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end() && !it->second->closed && it->second->engine) {
            auto* host = it->second->engine->windowHostById(id);
            if (host && !host->pendingClose) {
                s_childInboxes[id].push_back(std::move(m));
            }
        }
        return ev::undefined();
    });

    return b.get();
}

} // namespace

Value hostWindowOpener() {
    const uint64_t hostId = currentWindowHostId();
    if (hostId == 0) return ev::null();
    return openerProxyFor(hostId);
}

void installBroWindowParent(Value broWinIn) {
    // Rooted: building `parent` allocates, and broWinIn is a plain copy.
    ev::Persistent broWin(broWinIn);
    ObjectBuilder parent;
    parent.def("postMessage", 2, [](Value, std::span<const Value> a) -> Value {
        if (!isChildRealm()) {
            return ev::throwTypeError("bro.window.parent.postMessage is only available in a secondary window");
        }
        if (a.empty()) return ev::undefined();
        std::vector<ev::Persistent> transfers = collectTransferList(a);
        Message msg;
        const std::vector<Value> transferVals = currentValues(transfers);
        if (!serializeMessage(a[0], transferVals, msg)) {
            return ev::throwTypeError("postMessage: value is not cloneable");
        }
        dom::Document* curDoc = currentHostDocument();
        uint64_t hostId = 0;
        if (curDoc && hostEngine()) {
            if (auto* wh = hostEngine()->windowHostForDocument(curDoc)) {
                hostId = wh->id;
            }
        }
        if (hostId == 0) hostId = currentRealmScope();
        s_parentInbox.push_back({hostId, std::move(msg)});
        return ev::undefined();
    });
    ev::setProperty(broWin.get(), "parent", parent.get());
}

namespace {

// Opens a window host (hidden in headless, which is the engine's policy, not
// this layer's) and answers the handle both bro.window.open and window.open
// return; null when the engine has no primary window to share with.
Value openWindowHandle(engine::Engine* eng, const engine::WindowHostOptions& opts) {
    uint64_t id = eng->openWindowHost(opts);
    if (id == 0) return ev::null();

    auto state = std::make_shared<WindowHandleState>();
    state->engine = eng;
    state->id = id;
    state->width = opts.width;
    state->height = opts.height;
    state->x = opts.x;
    state->y = opts.y;
    state->title = opts.title;
    s_handleStates[id] = state;

    ev::Persistent handle(makeWindowHandle(state));
    s_windowHandles.emplace(id, handle.get());
    return handle.get();
}

// A URL with a scheme of its own (https:, mailto:, file:, ...) is for the OS,
// not a bro app directory. One letter before the colon is a drive (C:/...),
// which is a path.
bool isExternalUrl(const std::string& url) {
    size_t i = 0;
    if (url.empty() || !std::isalpha(static_cast<unsigned char>(url[0]))) return false;
    while (i < url.size() && (std::isalnum(static_cast<unsigned char>(url[i])) ||
                              url[i] == '+' || url[i] == '-' || url[i] == '.')) {
        ++i;
    }
    return i >= 2 && i < url.size() && url[i] == ':';
}

}  // namespace

void installBroWindowOpen(Value broWinIn) {
    ev::Persistent broWin(broWinIn);
    installBroWindowParent(broWin.get());

    ev::Persistent openFn(ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (isChildRealm()) {
            return ev::throwTypeError("bro.window.open is only available from the main app realm");
        }
        if (a.empty() || !ev::isString(a[0])) {
            return ev::throwTypeError("bro.window.open: src is required and must be a string");
        }
        std::string src = ev::toUtf8(a[0]);
        if (src.empty()) {
            return ev::throwTypeError("bro.window.open: src cannot be empty");
        }
        auto* eng = hostEngine();
        if (!eng) return ev::throwTypeError("bro.window.open: no engine available");

        engine::WindowHostOptions opts;
        opts.src = src;
        if (a.size() > 1 && ev::isObject(a[1])) {
            // Rooted: every getProperty below may run a getter and allocate.
            ev::Persistent o(a[1]);
            auto hasProp = [&](const char* k) {
                Value v = ev::getProperty(o.get(), k);
                return !ev::isUndefined(v) && !ev::isNull(v);
            };
            auto getInt = [&](const char* k, int def) {
                Value v = ev::getProperty(o.get(), k);
                return (!ev::isUndefined(v) && !ev::isNull(v)) ? static_cast<int>(ev::toDouble(v)) : def;
            };
            auto getBool = [&](const char* k, bool def) {
                Value v = ev::getProperty(o.get(), k);
                return (!ev::isUndefined(v) && !ev::isNull(v)) ? ev::toBool(v) : def;
            };
            auto getStr = [&](const char* k, const std::string& def) {
                Value v = ev::getProperty(o.get(), k);
                return (!ev::isUndefined(v) && !ev::isNull(v)) ? ev::toUtf8(v) : def;
            };

            if (hasProp("width"))       { opts.width = getInt("width", opts.width); opts.provided.width = true; }
            if (hasProp("height"))      { opts.height = getInt("height", opts.height); opts.provided.height = true; }
            if (hasProp("title"))       { opts.title = getStr("title", opts.title); opts.provided.title = true; }
            if (hasProp("x"))           opts.x = getInt("x", opts.x);
            if (hasProp("y"))           opts.y = getInt("y", opts.y);
            if (hasProp("display"))     opts.display = getInt("display", opts.display);
            if (hasProp("resizable"))   { opts.resizable = getBool("resizable", opts.resizable); opts.provided.resizable = true; }
            if (hasProp("borderless"))  { opts.borderless = getBool("borderless", opts.borderless); opts.provided.borderless = true; }
            if (hasProp("alwaysOnTop")) { opts.alwaysOnTop = getBool("alwaysOnTop", opts.alwaysOnTop); opts.provided.alwaysOnTop = true; }
            if (hasProp("minWidth"))    { opts.minWidth = getInt("minWidth", opts.minWidth); opts.provided.minWidth = true; }
            if (hasProp("minHeight"))   { opts.minHeight = getInt("minHeight", opts.minHeight); opts.provided.minHeight = true; }
            if (hasProp("maxWidth"))    { opts.maxWidth = getInt("maxWidth", opts.maxWidth); opts.provided.maxWidth = true; }
            if (hasProp("maxHeight"))   { opts.maxHeight = getInt("maxHeight", opts.maxHeight); opts.provided.maxHeight = true; }
        }
        if (opts.width < 1) opts.width = 1;
        if (opts.height < 1) opts.height = 1;

        Value handle = openWindowHandle(eng, opts);
        if (ev::isNull(handle)) {
            return ev::throwTypeError("bro.window.open: failed to open window host");
        }
        return handle;
    }, 2, "open"));
    ev::setProperty(broWin.get(), "open", openFn.get());
}

void windowHostNotifyLoaded(uint64_t id) {
    auto hIt = s_windowHandles.find(id);
    auto sIt = s_handleStates.find(id);
    if (hIt == s_windowHandles.end() || sIt == s_handleStates.end()) return;
    Value handle = hIt->second.get();
    ObjectBuilder evObj;
    evObj.set("type", ev::fromUtf8("load"));
    evObj.set("target", handle);
    Value evVal = evObj.get();
    auto listeners = sIt->second->loadListeners;
    for (auto& fn : listeners) {
        ev::call(fn.get(), handle, std::span<const Value>(&evVal, 1));
    }
}

void windowHostNotifyResized(uint64_t id, int width, int height) {
    auto hIt = s_windowHandles.find(id);
    auto sIt = s_handleStates.find(id);
    if (hIt == s_windowHandles.end() || sIt == s_handleStates.end()) return;
    sIt->second->width = width;
    sIt->second->height = height;
    Value handle = hIt->second.get();
    ObjectBuilder evObj;
    evObj.set("type", ev::fromUtf8("resize"));
    evObj.set("target", handle);
    evObj.set("width", ev::fromDouble(width));
    evObj.set("height", ev::fromDouble(height));
    Value evVal = evObj.get();
    auto listeners = sIt->second->resizeListeners;
    for (auto& fn : listeners) {
        ev::call(fn.get(), handle, std::span<const Value>(&evVal, 1));
    }
}

void windowHostNotifyClosed(uint64_t id) {
    auto hIt = s_windowHandles.find(id);
    auto sIt = s_handleStates.find(id);
    if (hIt == s_windowHandles.end() || sIt == s_handleStates.end()) return;
    sIt->second->closed = true;
    Value handle = hIt->second.get();
    ObjectBuilder evObj;
    evObj.set("type", ev::fromUtf8("close"));
    evObj.set("target", handle);
    Value evVal = evObj.get();
    auto listeners = sIt->second->closeListeners;
    s_windowHandles.erase(hIt);
    s_childInboxes.erase(id);
    s_openerProxies.erase(id);
    clearRealmScope(id);
    for (auto& fn : listeners) {
        ev::call(fn.get(), handle, std::span<const Value>(&evVal, 1));
    }
}

void windowHostNotifyMessage(uint64_t id, Value data) {
    auto hIt = s_windowHandles.find(id);
    auto sIt = s_handleStates.find(id);
    if (hIt == s_windowHandles.end() || sIt == s_handleStates.end()) return;
    Value handle = hIt->second.get();
    ObjectBuilder evObj;
    evObj.set("type", ev::fromUtf8("message"));
    evObj.set("target", handle);
    evObj.set("data", data);
    Value evVal = evObj.get();
    auto listeners = sIt->second->messageListeners;
    for (auto& fn : listeners) {
        ev::call(fn.get(), handle, std::span<const Value>(&evVal, 1));
    }
}

void drainHostWindowMessages() {
    auto* eng = hostEngine();
    if (!eng) return;

    // Phase 1: Children first — each a MessageEvent at the opened window,
    // with its `window.opener` as the source.
    if (!s_childInboxes.empty()) {
        auto inboxes = std::move(s_childInboxes);
        s_childInboxes.clear();

        for (auto& [hostId, batch] : inboxes) {
            for (auto& m : batch) {
                auto* host = eng->windowHostById(hostId);
                if (!host || host->pendingClose || !host->document) break;
                ev::Persistent source(openerProxyFor(hostId));
                deliverWindowMessageEvent(hostId, host->document.get(), m.data, m.ports, source,
                                          kHostPageOrigin);
            }
        }
    }

    // Phase 2: the main window, in post order — window.opener.postMessage
    // from a secondary window, a MessageEvent whose source is that window's
    // handle; then bro.window.parent.postMessage to the handles.
    if (!s_openerInbox.empty()) {
        auto batch = std::move(s_openerInbox);
        s_openerInbox.clear();
        for (auto& m : batch) {
            auto hIt = s_windowHandles.find(m.hostId);
            ev::Persistent source(hIt != s_windowHandles.end() ? hIt->second.get() : ev::null());
            deliverWindowMessageEvent(0, nullptr, m.data, m.ports, source, kHostPageOrigin);
        }
    }
    if (!s_parentInbox.empty()) {
        auto batch = std::move(s_parentInbox);
        s_parentInbox.clear();

        for (auto& pm : batch) {
            Value data = deserializeMessage(pm.msg);
            windowHostNotifyMessage(pm.hostId, data);
        }
    }
}

void resetWindowHostOpenState() {
    s_handleStates.clear();
    s_windowHandles.clear();
    s_childInboxes.clear();
    s_parentInbox.clear();
    s_openerInbox.clear();
    s_openerProxies.clear();
}

Value handleWindowOpen(std::span<const Value> a) {
    if (a.empty() || ev::isUndefined(a[0]) || ev::isNull(a[0])) return ev::null();
    std::string url = ev::toUtf8(a[0]);
    auto* e = hostEngine();
    if (!e) return ev::null();
    // No document to put in a blank window: bro windows are app directories.
    if (url.empty() || url == "about:blank") return ev::null();

    // A URL with its own scheme goes to the OS handler (browser, mail
    // client) and there is no window object to return. Headless never
    // shells out.
    if (isExternalUrl(url)) {
        if (e->displayMode() == engine::DisplayMode::Headless || !e->window()) {
            LOG_INFO("window.open('%s'): external URL not opened (headless)", url.c_str());
        } else if (!SDL_OpenURL(url.c_str())) {
            LOG_WARN("window.open('%s'): SDL_OpenURL failed: %s", url.c_str(), SDL_GetError());
        }
        return ev::null();
    }
    // A secondary window is only opened from the main realm, as
    // bro.window.open is.
    if (isChildRealm()) return ev::null();

    engine::WindowHostOptions opts;
    opts.src = url;
    // The second argument is the target name. The `_blank`/`_self`/...
    // keywords name no window; any other name titles the new one.
    if (a.size() > 1 && ev::isString(a[1])) {
        std::string target = ev::toUtf8(a[1]);
        if (!target.empty() && target[0] != '_') opts.title = target;
    }
    // `noopener` / `noreferrer`: the window opens, the caller gets null
    // (HTML's window open steps).
    bool noopener = false;
    if (a.size() > 2) {
        if (ev::isString(a[2])) {
            std::string feats = ev::toUtf8(a[2]);
            size_t start = 0;
            while (start < feats.size()) {
                size_t comma = feats.find(',', start);
                if (comma == std::string::npos) comma = feats.size();
                std::string token = feats.substr(start, comma - start);
                {
                    std::string t = token;
                    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(0, 1);
                    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
                    if (t == "noopener" || t == "noreferrer") noopener = true;
                }
                size_t eq = token.find('=');
                if (eq != std::string::npos) {
                    std::string k = token.substr(0, eq);
                    std::string v = token.substr(eq + 1);
                    while (!k.empty() && (k.front() == ' ' || k.front() == '\t')) k.erase(0, 1);
                    while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
                    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
                    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
                    try {
                        if (k == "width" || k == "innerWidth") {
                            opts.width = std::stoi(v);
                            opts.provided.width = true;
                        } else if (k == "height" || k == "innerHeight") {
                            opts.height = std::stoi(v);
                            opts.provided.height = true;
                        } else if (k == "left" || k == "screenX") {
                            opts.x = std::stoi(v);
                        } else if (k == "top" || k == "screenY") {
                            opts.y = std::stoi(v);
                        }
                    } catch (...) {}
                }
                start = comma + 1;
            }
        } else if (ev::isObject(a[2])) {
            // a[2] is a rooted argument slot, read afresh after each
            // allocating getProperty.
            Value w = ev::getProperty(a[2], "width");
            if (!ev::isUndefined(w) && !ev::isNull(w)) {
                opts.width = static_cast<int>(ev::toDouble(w));
                opts.provided.width = true;
            }
            Value h = ev::getProperty(a[2], "height");
            if (!ev::isUndefined(h) && !ev::isNull(h)) {
                opts.height = static_cast<int>(ev::toDouble(h));
                opts.provided.height = true;
            }
        }
    }
    if (opts.width < 1) opts.width = 1;
    if (opts.height < 1) opts.height = 1;

    Value handle = openWindowHandle(e, opts);
    return noopener ? ev::null() : handle;
}

} // namespace bro::bronze_host
