#include "bronze_host/host_window_open.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_worker_msg.h"
#include "bronze_host/host_realm_scope.h"
#include "engine/engine.h"
#include "platform/sdl_window.h"
#include "util/log.h"

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

static std::unordered_map<uint64_t, std::shared_ptr<WindowHandleState>> s_handleStates;
static std::unordered_map<uint64_t, ev::Persistent> s_windowHandles;
static std::unordered_map<uint64_t, std::vector<Message>> s_childInboxes;
static std::vector<ParentMessage> s_parentInbox;
static std::unordered_map<uint64_t, std::vector<ev::Persistent>> s_childMessageListeners;

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

    b.def("postMessage", 2, [id](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::undefined();
        Value val = a[0];
        std::span<const Value> transfers;
        std::vector<Value> transferVec;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value tList = a[1];
            Value lenVal = ev::getProperty(tList, "length");
            if (!ev::isUndefined(lenVal) && !ev::isNull(lenVal)) {
                int len = static_cast<int>(ev::toDouble(lenVal));
                for (int i = 0; i < len; ++i) {
                    transferVec.push_back(ev::getElement(tList, i));
                }
                transfers = transferVec;
            }
        }
        Message msg;
        if (!serializeMessage(val, transfers, msg)) {
            return ev::throwTypeError("postMessage: value is not cloneable");
        }
        auto it = s_handleStates.find(id);
        if (it != s_handleStates.end() && !it->second->closed && it->second->engine) {
            auto* host = it->second->engine->windowHostById(id);
            if (host && !host->pendingClose) {
                s_childInboxes[id].push_back(std::move(msg));
            }
        }
        return ev::undefined();
    });

    return b.get();
}

} // namespace

void addWindowHostChildMessageListener(uint64_t hostId, Value fn) {
    if (ev::isFunction(fn)) {
        s_childMessageListeners[hostId].emplace_back(fn);
    }
}

void removeWindowHostChildMessageListener(uint64_t hostId, Value fn) {
    auto it = s_childMessageListeners.find(hostId);
    if (it != s_childMessageListeners.end()) {
        auto& list = it->second;
        for (auto lit = list.begin(); lit != list.end(); ++lit) {
            if (lit->get() == fn) {
                list.erase(lit);
                break;
            }
        }
    }
}

void installBroWindowParent(Value broWin) {
    ObjectBuilder parent;
    parent.def("postMessage", 2, [](Value, std::span<const Value> a) -> Value {
        if (!isChildRealm()) {
            return ev::throwTypeError("bro.window.parent.postMessage is only available in a secondary window");
        }
        if (a.empty()) return ev::undefined();
        Value val = a[0];
        std::span<const Value> transfers;
        std::vector<Value> transferVec;
        if (a.size() > 1 && ev::isObject(a[1])) {
            Value tList = a[1];
            Value lenVal = ev::getProperty(tList, "length");
            if (!ev::isUndefined(lenVal) && !ev::isNull(lenVal)) {
                int len = static_cast<int>(ev::toDouble(lenVal));
                for (int i = 0; i < len; ++i) {
                    transferVec.push_back(ev::getElement(tList, i));
                }
                transfers = transferVec;
            }
        }
        Message msg;
        if (!serializeMessage(val, transfers, msg)) {
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
    ev::setProperty(broWin, "parent", parent.get());
}

void installBroWindowOpen(Value broWin) {
    installBroWindowParent(broWin);

    ev::setProperty(broWin, "open", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
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
            Value o = a[1];
            auto hasProp = [&](const char* k) {
                Value v = ev::getProperty(o, k);
                return !ev::isUndefined(v) && !ev::isNull(v);
            };
            auto getInt = [&](const char* k, int def) {
                Value v = ev::getProperty(o, k);
                return (!ev::isUndefined(v) && !ev::isNull(v)) ? static_cast<int>(ev::toDouble(v)) : def;
            };
            auto getBool = [&](const char* k, bool def) {
                Value v = ev::getProperty(o, k);
                return (!ev::isUndefined(v) && !ev::isNull(v)) ? ev::toBool(v) : def;
            };
            auto getStr = [&](const char* k, const std::string& def) {
                Value v = ev::getProperty(o, k);
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

        uint64_t id = eng->openWindowHost(opts);
        if (id == 0) return ev::throwTypeError("bro.window.open: failed to open window host");

        auto state = std::make_shared<WindowHandleState>();
        state->engine = eng;
        state->id = id;
        state->width = opts.width;
        state->height = opts.height;
        state->x = opts.x;
        state->y = opts.y;
        state->title = opts.title;
        s_handleStates[id] = state;

        Value handle = makeWindowHandle(state);
        s_windowHandles.emplace(id, handle);
        return handle;
    }, 2));
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
    s_childMessageListeners.erase(id);
    s_childInboxes.erase(id);
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

    // Phase 1: Children first
    if (!s_childInboxes.empty()) {
        auto inboxes = std::move(s_childInboxes);
        s_childInboxes.clear();

        for (auto& [hostId, batch] : inboxes) {
            auto* host = eng->windowHostById(hostId);
            if (!host || host->pendingClose || !host->document) continue;

            dom::Document* subDoc = host->document.get();
            dom::Document* prevDoc = currentHostDocument();
            ev::GlobalValue docG = ev::globalValue("document");
            ev::GlobalValue gt = ev::globalValue("globalThis");
            Value prevDocVal = docG.found ? docG.value : ev::null();

            enterRealmScope(hostId);
            setCurrentHostDocument(subDoc);
            Value subDocVal = hostDocumentValue(subDoc);
            ev::registerGlobal("document", subDocVal);
            if (gt.found && ev::isObject(gt.value)) {
                ev::setProperty(gt.value, "document", subDocVal);
            }

            for (auto& m : batch) {
                Value data = deserializeMessage(m);

                ObjectBuilder evt;
                evt.set("type", ev::fromUtf8("message"));
                evt.set("data", data);
                evt.set("target", gt.value);
                Value evtVal = evt.get();

                auto it = s_childMessageListeners.find(hostId);
                if (it != s_childMessageListeners.end()) {
                    auto listenersCopy = it->second;
                    for (auto& fn : listenersCopy) {
                        if (ev::isFunction(fn.get())) {
                            ev::CallResult r = ev::call(fn.get(), gt.value, std::span<const Value>(&evtVal, 1));
                            if (r.thrown) reportBronzeError("child window message listener", r.value);
                        }
                    }
                }

                Value onmessage = ev::getProperty(gt.value, "onmessage");
                if (ev::isFunction(onmessage)) {
                    ev::CallResult r = ev::call(onmessage, gt.value, std::span<const Value>(&evtVal, 1));
                    if (r.thrown) reportBronzeError("child window onmessage", r.value);
                }
            }

            if (!ev::isNull(prevDocVal)) {
                ev::registerGlobal("document", prevDocVal);
                if (gt.found && ev::isObject(gt.value)) {
                    ev::setProperty(gt.value, "document", prevDocVal);
                }
            }
            setCurrentHostDocument(prevDoc);
            exitRealmScope();
        }
    }

    // Phase 2: Parent second (in post order)
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
    s_childMessageListeners.clear();
}

} // namespace bro::bronze_host
