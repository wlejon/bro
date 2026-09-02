// localStorage / Storage implementation and proxy.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "util/storage_file.h"

#include <map>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// localStorage & AudioContext
// ---------------------------------------------------------------------------
struct StorageState {
    std::map<std::string, std::string> items;
    std::string path;
    bool loaded = false;
};

static StorageState g_storage;

// The file format — real JSON, written atomically — lives in
// util/storage_file.cpp, shared with the QuickJS Storage, so a bronze app and
// a script app read and write the same .storage.json.
static void loadStorageFile() {
    if (g_storage.loaded) return;
    g_storage.loaded = true;
    if (auto* eng = hostEngine()) {
        if (!eng->appDir().empty()) {
            g_storage.path = eng->appDir() + "/.storage.json";
        } else {
            g_storage.path = ".storage.json";
        }
    } else {
        g_storage.path = ".storage.json";
    }
    util::readStorageFile(g_storage.path, g_storage.items);
}

static void saveStorageFile() {
    if (g_storage.path.empty()) return;
    util::writeStorageFile(g_storage.path, g_storage.items);
}

}  // namespace

Value makeLocalStorageValue() {
    loadStorageFile();
    ObjectBuilder b;
    b.def("getItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (ev::isObject(keyV) || ev::isUndefined(keyV)) return ev::null();
        std::string key = ev::toUtf8(keyV);
        auto it = g_storage.items.find(key);
        if (it == g_storage.items.end()) return ev::null();
        return ev::fromUtf8(it->second);
    });
    b.def("setItem", 2, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        Value valV = argAt(a, 1);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            std::string key = ev::toUtf8(keyV);
            std::string val = (!ev::isObject(valV) && !ev::isUndefined(valV)) ? ev::toUtf8(valV) : "";
            g_storage.items[key] = val;
            saveStorageFile();
        }
        return ev::undefined();
    });
    b.def("removeItem", 1, [](Value, std::span<const Value> a) {
        Value keyV = argAt(a, 0);
        if (!ev::isObject(keyV) && !ev::isUndefined(keyV)) {
            g_storage.items.erase(ev::toUtf8(keyV));
            saveStorageFile();
        }
        return ev::undefined();
    });
    b.def("clear", 0, [](Value, std::span<const Value>) {
        g_storage.items.clear();
        saveStorageFile();
        return ev::undefined();
    });
    b.def("key", 1, [](Value, std::span<const Value> a) {
        int idx = i32At(a, 0);
        if (idx < 0 || static_cast<size_t>(idx) >= g_storage.items.size()) return ev::null();
        auto it = g_storage.items.begin();
        std::advance(it, idx);
        return ev::fromUtf8(it->first);
    });
    b.accessor("length", [](Value, std::span<const Value>) {
        return ev::fromDouble(static_cast<double>(g_storage.items.size()));
    }, nullptr);

    // Named properties over the same map getItem reads.
    // `localStorage.token` is how half the code on the web reads a stored
    // value, and a Storage object is specified as exactly that: named
    // properties over the same map getItem reads. The methods stay the
    // authority — they are what the proxy consults first — so nothing that
    // worked before changes shape.
    HostProxyTraps t;
    t.methods = b.get();
    t.get = [](const std::string& key, Value& out) {
        auto it = g_storage.items.find(key);
        if (it == g_storage.items.end()) return false;
        out = ev::fromUtf8(it->second);
        return true;
    };
    t.set = [](const std::string& key, Value v) {
        if (ev::isObject(v)) return;
        g_storage.items[key] = ev::isUndefined(v) ? "undefined" : ev::toUtf8(v);
        saveStorageFile();
    };
    t.has = [](const std::string& key) {
        return g_storage.items.find(key) != g_storage.items.end();
    };
    t.ownKeys = []() {
        std::vector<std::string> keys;
        for (const auto& [k, v] : g_storage.items) {
            (void)v;
            keys.push_back(k);
        }
        return keys;
    };
    t.remove = [](const std::string& key) {
        if (g_storage.items.erase(key)) saveStorageFile();
    };
    return makeHostProxy(std::move(t));
}

}  // namespace bro::bronze_host
