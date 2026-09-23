// localStorage / Storage implementation and proxy.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_storage.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/document.h"
#include "engine/engine.h"
#include "util/storage_file.h"

#include <map>
#include <mutex>
#include <string>
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

static StorageState& currentStorage() {
    std::string basePath = currentStorageBasePath();
    std::string path = basePath + "/.storage.json";
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
    std::string bp = basePath.empty() ? "." : basePath;
    std::string path = normalizePath(bp) + "/.storage.json";
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

Value makeLocalStorageValue() {
    ObjectBuilder b;
    b.def("getItem", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::null();
        std::string key = ev::toUtf8(a[0]);
        auto& st = currentStorage();
        auto it = st.items.find(key);
        if (it == st.items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value, std::span<const Value> a) {
        std::string key = a.empty() ? "undefined" : ev::toUtf8(a[0]);
        std::string val = a.size() < 2 ? "undefined" : ev::toUtf8(a[1]);
        auto& st = currentStorage();
        auto it = st.items.find(key);
        std::string oldVal;
        bool hasOld = (it != st.items.end());
        if (hasOld) oldVal = it->second;
        st.items[key] = val;
        scheduleSaveStorage(st);
        dispatchStorageEvent(/*isSession=*/false, &key, hasOld ? &oldVal : nullptr, &val);
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::undefined();
        std::string key = ev::toUtf8(a[0]);
        auto& st = currentStorage();
        auto it = st.items.find(key);
        if (it != st.items.end()) {
            std::string oldVal = it->second;
            st.items.erase(it);
            scheduleSaveStorage(st);
            dispatchStorageEvent(/*isSession=*/false, &key, &oldVal, nullptr);
        }
        return ev::undefined();
    });
    b.def("clear", 0, [](Value, std::span<const Value>) {
        auto& st = currentStorage();
        if (!st.items.empty()) {
            st.items.clear();
            scheduleSaveStorage(st);
            dispatchStorageEvent(/*isSession=*/false, nullptr, nullptr, nullptr);
        }
        return ev::undefined();
    });
    b.def("key", 1, [](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        auto& st = currentStorage();
        if (idx < 0 || static_cast<size_t>(idx) >= st.items.size()) return ev::null();
        auto it = st.items.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });
    b.accessor("length", [](Value, std::span<const Value>) {
        return ev::fromDouble(static_cast<double>(currentStorage().items.size()));
    }, nullptr);

    // Named properties over the same map getItem reads.
    HostProxyTraps t;
    t.methods = b.get();
    t.get = [](const std::string& key, Value& out) {
        auto& st = currentStorage();
        auto it = st.items.find(key);
        if (it == st.items.end()) return false;
        out = ev::fromUtf8(it->second);
        return true;
    };
    t.set = [](const std::string& key, Value v) {
        auto& st = currentStorage();
        auto it = st.items.find(key);
        std::string oldVal;
        bool hasOld = (it != st.items.end());
        if (hasOld) oldVal = it->second;
        std::string val = ev::isUndefined(v) ? "undefined" : ev::toUtf8(v);
        st.items[key] = val;
        scheduleSaveStorage(st);
        dispatchStorageEvent(/*isSession=*/false, &key, hasOld ? &oldVal : nullptr, &val);
    };
    t.has = [](const std::string& key) {
        return currentStorage().items.find(key) != currentStorage().items.end();
    };
    t.ownKeys = []() {
        std::vector<std::string> keys;
        for (const auto& [k, v] : currentStorage().items) {
            (void)v;
            keys.push_back(k);
        }
        return keys;
    };
    t.remove = [](const std::string& key) {
        auto& st = currentStorage();
        auto it = st.items.find(key);
        if (it != st.items.end()) {
            std::string oldVal = it->second;
            st.items.erase(it);
            scheduleSaveStorage(st);
            dispatchStorageEvent(/*isSession=*/false, &key, &oldVal, nullptr);
        }
    };
    return makeHostProxy(std::move(t));
}

Value makeSessionStorageValue() {
    ObjectBuilder b;
    b.def("getItem", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::null();
        std::string key = ev::toUtf8(a[0]);
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        if (it == st.items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value, std::span<const Value> a) {
        std::string key = a.empty() ? "undefined" : ev::toUtf8(a[0]);
        std::string val = a.size() < 2 ? "undefined" : ev::toUtf8(a[1]);
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        std::string oldVal;
        bool hasOld = (it != st.items.end());
        if (hasOld) oldVal = it->second;
        st.items[key] = val;
        dispatchStorageEvent(/*isSession=*/true, &key, hasOld ? &oldVal : nullptr, &val);
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value, std::span<const Value> a) {
        if (a.empty()) return ev::undefined();
        std::string key = ev::toUtf8(a[0]);
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        if (it != st.items.end()) {
            std::string oldVal = it->second;
            st.items.erase(it);
            dispatchStorageEvent(/*isSession=*/true, &key, &oldVal, nullptr);
        }
        return ev::undefined();
    });
    b.def("clear", 0, [](Value, std::span<const Value>) {
        auto& st = currentSessionStorage();
        if (!st.items.empty()) {
            st.items.clear();
            dispatchStorageEvent(/*isSession=*/true, nullptr, nullptr, nullptr);
        }
        return ev::undefined();
    });
    b.def("key", 1, [](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        auto& st = currentSessionStorage();
        if (idx < 0 || static_cast<size_t>(idx) >= st.items.size()) return ev::null();
        auto it = st.items.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });
    b.accessor("length", [](Value, std::span<const Value>) {
        return ev::fromDouble(static_cast<double>(currentSessionStorage().items.size()));
    }, nullptr);

    HostProxyTraps t;
    t.methods = b.get();
    t.get = [](const std::string& key, Value& out) {
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        if (it == st.items.end()) return false;
        out = ev::fromUtf8(it->second);
        return true;
    };
    t.set = [](const std::string& key, Value v) {
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        std::string oldVal;
        bool hasOld = (it != st.items.end());
        if (hasOld) oldVal = it->second;
        std::string val = ev::isUndefined(v) ? "undefined" : ev::toUtf8(v);
        st.items[key] = val;
        dispatchStorageEvent(/*isSession=*/true, &key, hasOld ? &oldVal : nullptr, &val);
    };
    t.has = [](const std::string& key) {
        return currentSessionStorage().items.find(key) != currentSessionStorage().items.end();
    };
    t.ownKeys = []() {
        std::vector<std::string> keys;
        for (const auto& [k, v] : currentSessionStorage().items) {
            (void)v;
            keys.push_back(k);
        }
        return keys;
    };
    t.remove = [](const std::string& key) {
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        if (it != st.items.end()) {
            std::string oldVal = it->second;
            st.items.erase(it);
            dispatchStorageEvent(/*isSession=*/true, &key, &oldVal, nullptr);
        }
    };
    return makeHostProxy(std::move(t));
}

}  // namespace bro::bronze_host
