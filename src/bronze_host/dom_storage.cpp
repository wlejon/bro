// localStorage / Storage implementation and proxy.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_storage.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/document.h"
#include "engine/engine.h"
#include "util/storage_file.h"

#include <map>
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

}  // namespace

void reloadHostStorage(const std::string& basePath) {
    std::string bp = basePath.empty() ? "." : basePath;
    std::string path = normalizePath(bp) + "/.storage.json";
    auto& st = g_storages[path];
    st.path = path;
    st.loaded = true;
    st.items.clear();
    util::readStorageFile(st.path, st.items);
}

Value makeLocalStorageValue() {
    ObjectBuilder b;
    b.def("getItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (ev::isObject(keyV) || ev::isUndefined(keyV)) return ev::null();
        std::string key = ev::toUtf8(keyV);
        auto& st = currentStorage();
        auto it = st.items.find(key);
        if (it == st.items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        Value valV = argAt(a, 1);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            std::string key = ev::toUtf8(keyV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            auto& st = currentStorage();
            st.items[key] = val;
            saveStorage(st);
        }
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            auto& st = currentStorage();
            st.items.erase(ev::toUtf8(keyV));
            saveStorage(st);
        }
        return ev::undefined();
    });
    b.def("clear", 0, [](Value, std::span<const Value>) {
        auto& st = currentStorage();
        st.items.clear();
        saveStorage(st);
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
        if (ev::isObject(v)) return;
        auto& st = currentStorage();
        st.items[key] = ev::isUndefined(v) ? "undefined" : ev::toUtf8(v);
        saveStorage(st);
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
        if (st.items.erase(key)) saveStorage(st);
    };
    return makeHostProxy(std::move(t));
}

Value makeSessionStorageValue() {
    ObjectBuilder b;
    b.def("getItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (ev::isObject(keyV) || ev::isUndefined(keyV)) return ev::null();
        std::string key = ev::toUtf8(keyV);
        auto& st = currentSessionStorage();
        auto it = st.items.find(key);
        if (it == st.items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        Value valV = argAt(a, 1);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            std::string key = ev::toUtf8(keyV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            currentSessionStorage().items[key] = val;
        }
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            currentSessionStorage().items.erase(ev::toUtf8(keyV));
        }
        return ev::undefined();
    });
    b.def("clear", 0, [](Value, std::span<const Value>) {
        currentSessionStorage().items.clear();
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
        if (ev::isObject(v)) return;
        currentSessionStorage().items[key] = ev::isUndefined(v) ? "undefined" : ev::toUtf8(v);
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
        currentSessionStorage().items.erase(key);
    };
    return makeHostProxy(std::move(t));
}

}  // namespace bro::bronze_host
