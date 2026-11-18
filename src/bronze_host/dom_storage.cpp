// localStorage / Storage implementation and proxy.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_storage.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_events.h"
#include "bronze_host/host_proxy.h"
#include "bronze_host/host_web_globals.h"

#include "dom/document.h"
#include "engine/app_manifest.h"
#include "engine/engine.h"
#include "util/storage_file.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// localStorage & sessionStorage
// ---------------------------------------------------------------------------
struct StorageState {
    std::map<std::string, std::string> items;
    std::string path;
    bool loaded = false;
    bool dirty = false;
    bool saveScheduled = false;
};

static std::map<std::string, StorageState> g_storages;
static std::map<std::string, StorageState> g_sessionStorages;

static std::string normalizePath(std::string p) {
    for (char& c : p) {
        if (c == '\\') c = '/';
    }
    return p;
}

static std::string currentStorageBasePath() {
    std::string basePath;
    dom::Document* curDoc = currentHostDocument();
    if (curDoc && !curDoc->basePath().empty()) {
        basePath = curDoc->basePath();
    } else if (auto* eng = hostEngine()) {
        if (eng->document() && !eng->document()->basePath().empty()) {
            basePath = eng->document()->basePath();
        } else if (!eng->appDir().empty()) {
            basePath = eng->appDir();
        }
    }
    if (basePath.empty()) basePath = ".";
    return normalizePath(basePath);
}

static std::string stripTrailingSlash(std::string s) {
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\') &&
           !(s.size() == 3 && s[1] == ':')) {
        s.pop_back();
    }
    return s;
}

static std::string storageFilePathFor(const std::string& rawBasePath) {
    const std::string bp = rawBasePath.empty() ? "." : rawBasePath;
    const std::string scratchHome = engine::scratchProfileHome();
    if (scratchHome.empty()) {
        return normalizePath(bp) + "/.storage.json";
    }
    namespace fs = std::filesystem;
    const fs::path dataRoot = fs::path(scratchHome) / "data";
    if (auto* eng = hostEngine(); eng && !eng->appDir().empty()) {
        std::error_code ec;
        fs::path appAbs = fs::weakly_canonical(fs::path(eng->appDir()), ec);
        if (ec) appAbs = fs::absolute(fs::path(eng->appDir()), ec).lexically_normal();
        ec.clear();
        fs::path docAbs = fs::weakly_canonical(fs::path(bp), ec);
        if (ec) docAbs = fs::absolute(fs::path(bp), ec).lexically_normal();
        appAbs = fs::path(stripTrailingSlash(normalizePath(appAbs.string())));
        docAbs = fs::path(stripTrailingSlash(normalizePath(docAbs.string())));
        fs::path rel = docAbs.lexically_relative(appAbs);
        if (!rel.empty() && rel != "." && *rel.begin() != "..") {
            return normalizePath((dataRoot / rel / ".storage.json").string());
        }
    }
    return normalizePath((dataRoot / ".storage.json").string());
}

static StorageState& currentStorage() {
    std::string path = storageFilePathFor(currentStorageBasePath());
    auto& st = g_storages[path];
    if (!st.loaded) {
        st.path = path;
        st.loaded = true;
        util::readStorageFile(st.path, st.items);
    }
    return st;
}

static StorageState& currentSessionStorage() {
    std::string basePath = currentStorageBasePath();
    return g_sessionStorages[basePath];
}

static void saveStorage(StorageState& st) {
    if (st.path.empty()) return;
    util::writeStorageFile(st.path, st.items);
}

static void scheduleSaveStorage(StorageState& st) {
    st.dirty = true;
    static std::once_flag atexitOnce;
    std::call_once(atexitOnce, []() {
        std::atexit(flushHostStorage);
    });
    if (st.saveScheduled) return;
    st.saveScheduled = true;
    std::string path = st.path;
    postHostTask([path]() {
        auto it = g_storages.find(path);
        if (it != g_storages.end()) {
            it->second.saveScheduled = false;
            if (it->second.dirty) {
                it->second.dirty = false;
                saveStorage(it->second);
            }
        }
    });
}

}  // namespace

void flushHostStorage() {
    for (auto& [path, st] : g_storages) {
        if (st.dirty) {
            st.dirty = false;
            st.saveScheduled = false;
            saveStorage(st);
        }
    }
}

void reloadHostStorage(const std::string& basePath) {
    std::string path = storageFilePathFor(basePath);
    auto& st = g_storages[path];
    if (st.dirty) {
        st.dirty = false;
        st.saveScheduled = false;
        saveStorage(st);
    }
    st.path = path;
    st.loaded = true;
    st.items.clear();
    util::readStorageFile(st.path, st.items);
}

static void dispatchStorageEvent(bool isSession,
                                 const std::string* key,
                                 const std::string* oldVal,
                                 const std::string* newVal) {
    ObjectBuilder evObj;
    evObj.set("type", ev::fromUtf8("storage"));
    evObj.set("bubbles", ev::fromBool(false));
    evObj.set("cancelable", ev::fromBool(false));
    evObj.set("isTrusted", ev::fromBool(true));
    if (key) {
        evObj.set("key", ev::fromUtf8(*key));
    } else {
        evObj.set("key", ev::null());
    }
    if (oldVal) {
        evObj.set("oldValue", ev::fromUtf8(*oldVal));
    } else {
        evObj.set("oldValue", ev::null());
    }
    if (newVal) {
        evObj.set("newValue", ev::fromUtf8(*newVal));
    } else {
        evObj.set("newValue", ev::null());
    }

    std::string urlStr;
    ev::GlobalValue loc = ev::globalValue("location");
    if (loc.found && ev::isObject(loc.value)) {
        Value hrefVal = ev::getProperty(loc.value, "href");
        if (ev::isString(hrefVal)) urlStr = ev::toUtf8(hrefVal);
    }
    evObj.set("url", ev::fromUtf8(urlStr));

    ev::GlobalValue storageGlobal = ev::globalValue(isSession ? "sessionStorage" : "localStorage");
    if (storageGlobal.found && ev::isObject(storageGlobal.value)) {
        evObj.set("storageArea", storageGlobal.value);
    } else {
        evObj.set("storageArea", ev::null());
    }

    // The event object is re-read from its builder after the constructor
    // lookup, which may allocate.
    ev::GlobalValue seCtor = ev::globalValue("StorageEvent");
    if (seCtor.found && ev::isObject(seCtor.value)) {
        Value proto = ev::getProperty(seCtor.value, "prototype");
        if (ev::isObject(proto)) {
            evObj.obj.set(ev::setPrototype(evObj.get(), proto));
        }
    }
    hostDispatchToWindow(evObj.get());
}

namespace {

// The key a Storage proxy answers with its kind ("local" / "session"), which
// Storage.prototype's members read to find their map. Not a key a page can
// write: the proxy answers it before the items.
constexpr const char* kStorageKindKey = "\x01bro:storage";

StorageState& storageOf(bool isSession) { return isSession ? currentSessionStorage() : currentStorage(); }

void writeItem(bool isSession, const std::string& key, const std::string& val) {
    StorageState& st = storageOf(isSession);
    auto it = st.items.find(key);
    const bool hasOld = it != st.items.end();
    const std::string oldVal = hasOld ? it->second : std::string();
    st.items[key] = val;
    if (!isSession) scheduleSaveStorage(st);
    dispatchStorageEvent(isSession, &key, hasOld ? &oldVal : nullptr, &val);
}

void removeItem(bool isSession, const std::string& key) {
    StorageState& st = storageOf(isSession);
    auto it = st.items.find(key);
    if (it == st.items.end()) return;
    const std::string oldVal = it->second;
    st.items.erase(it);
    if (!isSession) scheduleSaveStorage(st);
    dispatchStorageEvent(isSession, &key, &oldVal, nullptr);
}

// Which storage `self` is; throws "Illegal invocation" for anything else.
bool storageKindOf(Value self) {
    if (ev::isObject(self)) {
        Value kind = ev::getProperty(self, kStorageKindKey);
        if (ev::isString(kind)) return ev::toUtf8(kind) == "session";
    }
    ev::throwTypeError("Illegal invocation");
}

std::string argString(std::span<const Value> a, size_t i) {
    return i < a.size() ? ev::toUtf8(a[i]) : std::string("undefined");
}

void decorateStorageProto(ObjectBuilder& b) {
    b.def("getItem", 1, [](Value self, std::span<const Value> a) {
        StorageState& st = storageOf(storageKindOf(self));
        auto it = st.items.find(argString(a, 0));
        return it == st.items.end() ? ev::null() : ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value self, std::span<const Value> a) {
        const bool session = storageKindOf(self);
        writeItem(session, argString(a, 0), argString(a, 1));
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value self, std::span<const Value> a) {
        const bool session = storageKindOf(self);
        removeItem(session, argString(a, 0));
        return ev::undefined();
    });
    b.def("clear", 0, [](Value self, std::span<const Value>) {
        const bool session = storageKindOf(self);
        StorageState& st = storageOf(session);
        if (!st.items.empty()) {
            st.items.clear();
            if (!session) scheduleSaveStorage(st);
            dispatchStorageEvent(session, nullptr, nullptr, nullptr);
        }
        return ev::undefined();
    });
    b.def("key", 1, [](Value self, std::span<const Value> a) {
        StorageState& st = storageOf(storageKindOf(self));
        const int idx = i32At(a, 0);
        if (idx < 0 || static_cast<size_t>(idx) >= st.items.size()) return ev::null();
        auto it = st.items.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });
    b.accessor("length", [](Value self, std::span<const Value>) {
        return ev::fromDouble(static_cast<double>(storageOf(storageKindOf(self)).items.size()));
    }, nullptr);
}

HostClass g_storageClass;

// localStorage / sessionStorage: a Storage whose named properties are the
// items (`localStorage.k = v`, `Object.keys(localStorage)`), its members on
// Storage.prototype as on the web.
Value makeStorageValue(bool isSession) {
    HostProxyTraps t;
    t.proto = g_storageClass.prototype();
    t.get = [isSession](const std::string& key, Value& out) {
        if (key == kStorageKindKey) {
            out = ev::fromUtf8(isSession ? "session" : "local");
            return true;
        }
        StorageState& st = storageOf(isSession);
        auto it = st.items.find(key);
        if (it == st.items.end()) return false;
        out = ev::fromUtf8(it->second);
        return true;
    };
    t.set = [isSession](const std::string& key, Value v) {
        writeItem(isSession, key, ev::isUndefined(v) ? "undefined" : ev::toUtf8(v));
    };
    t.has = [isSession](const std::string& key) {
        const StorageState& st = storageOf(isSession);
        return st.items.find(key) != st.items.end();
    };
    t.ownKeys = [isSession]() {
        std::vector<std::string> keys;
        for (const auto& [k, v] : storageOf(isSession).items) {
            (void)v;
            keys.push_back(k);
        }
        return keys;
    };
    t.remove = [isSession](const std::string& key) { removeItem(isSession, key); };
    return makeHostProxy(std::move(t));
}

}  // namespace

// The window installs localStorage first, once per realm: the interface
// object goes in with it.
Value makeLocalStorageValue() {
    g_storageClass.install("Storage", 0, [](Value, std::span<const Value>) -> Value {
        return ev::throwTypeError("Illegal constructor");
    }, decorateStorageProto);
    return makeStorageValue(false);
}
Value makeSessionStorageValue() { return makeStorageValue(true); }

}  // namespace bro::bronze_host
