// localStorage / Storage implementation and proxy.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"

#include <fstream>
#include <map>
#include <sstream>
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

static std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

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

    std::ifstream file(g_storage.path);
    if (!file.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    size_t pos = content.find('{');
    if (pos == std::string::npos) return;
    pos++;

    auto parseString = [&](size_t& p) -> std::string {
        if (p >= content.size() || content[p] != '"') return "";
        p++; // skip opening quote
        std::string result;
        while (p < content.size() && content[p] != '"') {
            if (content[p] == '\\' && p + 1 < content.size()) {
                p++;
                switch (content[p]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    default: result += content[p]; break;
                }
            } else {
                result += content[p];
            }
            p++;
        }
        if (p < content.size()) p++; // skip closing quote
        return result;
    };

    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
            pos++;
        if (pos >= content.size() || content[pos] == '}') break;

        std::string key = parseString(pos);
        while (pos < content.size() && content[pos] != ':') pos++;
        if (pos < content.size()) pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

        std::string value = parseString(pos);
        if (!key.empty()) {
            g_storage.items[key] = value;
        }
    }
}

static void saveStorageFile() {
    if (g_storage.path.empty()) return;
    std::ofstream file(g_storage.path);
    if (!file.is_open()) return;
    file << "{\n";
    size_t idx = 0;
    for (const auto& [k, v] : g_storage.items) {
        if (idx > 0) file << ",\n";
        file << "  \"" << escapeJsonString(k) << "\": \"" << escapeJsonString(v) << "\"";
        idx++;
    }
    file << "\n}\n";
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
