#include "engine/grant_store.h"

#include "engine/app_manifest.h"
#include "engine/capabilities.h"
#include "engine/install_record.h"
#include "util/json_in.h"
#include "util/json_out.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string getEnv(const char* name) {
#ifdef _WIN32
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    buf.resize(n);
    return u8str(fs::path(buf));
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

// Paths compare as the file system does: case-blind on Windows.
std::string comparable(const fs::path& p) {
    std::string s = u8str(p.lexically_normal());
    while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return s;
}

Decision parseDecision(const std::string& s) {
    if (s == "allow") return Decision::Allow;
    if (s == "deny") return Decision::Deny;
    return Decision::Undecided;
}

}  // namespace

const char* decisionName(Decision d) {
    switch (d) {
        case Decision::Allow: return "allow";
        case Decision::Deny: return "deny";
        default: return "undecided";
    }
}

namespace {
std::string findGrantStoreDir();
}

// Settled once, at the first question (the engine asks before any script
// runs): an app that may change the environment must not move the store it
// is judged by.
std::string grantStoreDir() {
    static const std::string dir = findGrantStoreDir();
    return dir;
}

namespace {

std::string findGrantStoreDir() {
    const std::string appHome = getEnv("BRO_APP_HOME");
    if (!appHome.empty()) return u8str((u8path(appHome) / "bro").lexically_normal());
#if defined(_WIN32)
    std::string base = getEnv("APPDATA");
    if (base.empty()) {
        const std::string profile = getEnv("USERPROFILE");
        base = profile.empty() ? std::string(".") : u8str(u8path(profile) / "AppData" / "Roaming");
    }
    return u8str(u8path(base) / "bro");
#elif defined(__APPLE__)
    const std::string h = getEnv("HOME");
    return u8str(u8path(h.empty() ? std::string(".") : h) / "Library" / "Application Support" / "bro");
#else
    std::string base = getEnv("XDG_CONFIG_HOME");
    if (base.empty() || !u8path(base).is_absolute()) {
        const std::string h = getEnv("HOME");
        base = u8str(u8path(h.empty() ? std::string(".") : h) / ".config");
    }
    return u8str(u8path(base) / "bro");
#endif
}

}  // namespace

std::string grantStorePath() { return u8str(u8path(grantStoreDir()) / "grants.json"); }

GrantStoreData loadGrantStore(bool* readOk) {
    GrantStoreData out;
    if (readOk) *readOk = true;
    const std::string path = grantStorePath();
    std::ifstream file(u8path(path), std::ios::binary);
    if (!file.is_open()) {
        // No store is an empty one; one there but not readable (another
        // process renaming its new copy into place) is not.
        std::error_code ec;
        if (readOk && fs::exists(u8path(path), ec)) *readOk = false;
        return out;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    util::JsonValue root;
    std::string err;
    if (!util::parseJson(ss.str(), root, &err) || !root.isObject()) {
        if (readOk) *readOk = false;
        LOG_WARN("%s: %s; no app is granted anything from it", path.c_str(),
                 err.empty() ? "not a JSON object" : err.c_str());
        return out;
    }
    if (const util::JsonValue* d = root.find("devMode"); d && d->type == util::JsonValue::Type::Bool)
        out.devMode = d->boolean;
    if (const util::JsonValue* apps = root.find("apps"); apps && apps->isObject()) {
        for (const auto& [identity, entry] : apps->object) {
            if (!entry.isObject()) continue;
            AppGrantRecord rec;
            rec.id = entry.get("id").string;
            rec.dir = entry.get("dir").string;
            rec.installHash = entry.get("installHash").string;
            if (const util::JsonValue* caps = entry.find("capabilities"); caps && caps->isObject())
                for (const auto& [cap, v] : caps->object)
                    if (v.isString()) rec.decisions[cap] = parseDecision(v.string);
            if (const util::JsonValue* picked = entry.find("picked"); picked && picked->isArray())
                for (const util::JsonValue& p : picked->array)
                    if (p.isString() && !p.string.empty()) rec.picked.push_back(p.string);
            out.apps.emplace(identity, std::move(rec));
        }
    }
    return out;
}

namespace {

std::string serialize(const GrantStoreData& data) {
    util::JsonOut j;
    j.beginObject();
    j.key("devMode").boolean(data.devMode);
    j.key("apps").beginObject();
    for (const auto& [identity, rec] : data.apps) {
        j.key(identity).beginObject();
        j.key("id").string(rec.id);
        j.key("dir").string(rec.dir);
        if (!rec.installHash.empty()) j.key("installHash").string(rec.installHash);
        j.key("capabilities").beginObject();
        for (const auto& [cap, d] : rec.decisions)
            if (d != Decision::Undecided) j.key(cap).string(decisionName(d));
        j.endObject();
        j.key("picked").beginArray();
        for (const auto& p : rec.picked) j.string(p);
        j.endArray();
        j.endObject();
    }
    j.endObject();
    j.endObject();
    return j.str() + "\n";
}

}  // namespace

namespace {

// Written beside the file and renamed over it, so a reader in another bro
// process sees the old file or the new one, never half of one.
bool replaceFile(const fs::path& path, const std::string& text, std::string* error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const fs::path tmp = path.parent_path() / u8path(u8str(path.filename()) + "." + std::to_string(pid) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (error) *error = "cannot write " + u8str(tmp);
            return false;
        }
        out << text;
        if (!out.good()) {
            if (error) *error = "cannot write " + u8str(tmp);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    // On Windows a reader that has the file open (a running app reading what
    // changed) makes the rename fail for that moment: try again briefly.
    for (int attempt = 0; ec && attempt < 100; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        if (error) *error = "cannot replace " + u8str(path) + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

bool updateGrantStore(const std::function<void(GrantStoreData&)>& change, std::string* error) {
    GrantStoreData data = loadGrantStore();
    change(data);
    return replaceFile(u8path(grantStorePath()), serialize(data), error);
}

std::string grantUsagePath() { return u8str(u8path(grantStoreDir()) / "usage.json"); }

std::map<std::string, std::map<std::string, double>> loadGrantUsage() {
    std::map<std::string, std::map<std::string, double>> out;
    std::ifstream file(u8path(grantUsagePath()), std::ios::binary);
    if (!file.is_open()) return out;
    std::ostringstream ss;
    ss << file.rdbuf();
    util::JsonValue root;
    if (!util::parseJson(ss.str(), root) || !root.isObject()) return out;
    for (const auto& [identity, caps] : root.object) {
        if (!caps.isObject()) continue;
        for (const auto& [cap, t] : caps.object)
            if (t.isNumber()) out[identity][cap] = t.number;
    }
    return out;
}

bool mergeGrantUsage(const std::string& identity, const std::map<std::string, double>& used, std::string* error) {
    auto all = loadGrantUsage();
    auto& mine = all[identity];
    for (const auto& [cap, t] : used) mine[cap] = (std::max)(mine[cap], t);
    util::JsonOut j;
    j.beginObject();
    for (const auto& [id, caps] : all) {
        j.key(id).beginObject();
        for (const auto& [cap, t] : caps) j.key(cap).number(t, 0);
        j.endObject();
    }
    j.endObject();
    return replaceFile(u8path(grantUsagePath()), j.str() + "\n", error);
}

std::string appIdentityFor(const std::string& appDir) {
    if (appDir.empty()) return {};
    std::error_code ec;
    fs::path abs = fs::absolute(u8path(appDir), ec).lexically_normal();
    if (ec) abs = u8path(appDir).lexically_normal();
    if (!abs.has_filename()) abs = abs.parent_path();
    // An install root's folder, as reached (a `--link` install is a symlink in
    // the root: the link's name is the record, wherever it points).
    const std::string parent = comparable(abs.parent_path());
    const std::string name = u8str(abs.filename());
    if (isValidAppId(name)) {
        for (const std::string& root : installedAppRoots()) {
            fs::path r = u8path(root);
            fs::path rc = fs::weakly_canonical(r, ec);
            if (comparable(r) == parent || (!ec && comparable(rc) == parent)) return "install:" + name;
        }
    }
    fs::path canon = fs::weakly_canonical(abs, ec);
    return "path:" + u8str((ec ? abs : canon).lexically_normal());
}

std::string resolveIdentityArg(const std::string& idOrDir, const GrantStoreData& store) {
    if (idOrDir.empty()) return {};
    std::error_code ec;
    if (fs::is_directory(u8path(idOrDir), ec)) return appIdentityFor(idOrDir);
    if (isValidAppId(idOrDir)) {
        const std::string installed = findInstalledApp(idOrDir);
        if (!installed.empty()) return appIdentityFor(installed);
        if (store.apps.count("install:" + idOrDir)) return "install:" + idOrDir;
        for (const auto& [identity, rec] : store.apps)
            if (rec.id == idOrDir) return identity;
    }
    // An identity as listAppPermissions spells it, though the store may not
    // hold it yet (an installed app nothing has been decided for).
    if (store.apps.count(idOrDir) || idOrDir.rfind("install:", 0) == 0 || idOrDir.rfind("path:", 0) == 0)
        return idOrDir;
    return {};
}

namespace {

void readDeclared(const std::string& dir, AppPermissionsEntry& e) {
    if (dir.empty()) return;
    AppDescriptor m;
    std::error_code ec;
    const fs::path json = u8path(dir) / "bro.json";
    if (!fs::exists(json, ec) || !parseAppManifest(u8str(json), m, nullptr)) return;
    e.declared = m.permissions;
    if (e.name.empty()) e.name = m.name;
    if (e.id.empty()) e.id = appIdFor(m, dir);
}

}  // namespace

std::vector<AppPermissionsEntry> listAppPermissions() {
    const GrantStoreData store = loadGrantStore();
    std::map<std::string, AppPermissionsEntry> byIdentity;
    for (const std::string& root : installedAppRoots()) {
        std::error_code ec;
        for (auto& d : fs::directory_iterator(u8path(root), ec)) {
            if (!d.is_directory(ec)) continue;
            const std::string name = u8str(d.path().filename());
            if (!isValidAppId(name) || name.ends_with(".new") || name.ends_with(".old")) continue;
            const std::string identity = "install:" + name;
            if (byIdentity.count(identity)) continue;   // the user's copy shadows the system's
            AppPermissionsEntry e;
            e.identity = identity;
            e.id = name;
            e.dir = u8str(d.path());
            e.installed = true;
            byIdentity.emplace(identity, std::move(e));
        }
    }
    for (const auto& [identity, rec] : store.apps) {
        AppPermissionsEntry& e = byIdentity[identity];
        e.identity = identity;
        if (e.id.empty()) e.id = rec.id;
        if (e.dir.empty()) e.dir = rec.dir;
        e.decisions = rec.decisions;
        e.picked = rec.picked;
    }
    const auto usage = loadGrantUsage();
    std::vector<AppPermissionsEntry> out;
    for (auto& [identity, e] : byIdentity) {
        readDeclared(e.dir, e);
        if (auto u = usage.find(identity); u != usage.end()) e.lastUsed = u->second;
        if (identity.rfind("install:", 0) == 0) {
            // The record's hash, not the folder's: what a launch checks
            // (install_record.h) costs a walk of the folder, and a listing
            // must stay cheap.
            InstallRecord rec;
            const bool recorded = !e.dir.empty() && readInstallRecord(e.dir, rec);
            const std::string current = recorded ? rec.hash : std::string("unrecorded");
            // A store record for other content is a change, decided or not:
            // the launch that found it has already dropped its decisions.
            auto s = store.apps.find(identity);
            const bool known = s != store.apps.end() && !s->second.installHash.empty();
            if (known && s->second.installHash != current) e.install = "changed";
            else if (!recorded) e.install = "unrecorded";
            else if (rec.linked) e.install = "linked";
            else e.install = "current";
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::string currentInstallHash(const std::string& identity, const std::string& dir) {
    if (identity.rfind("install:", 0) != 0) return {};
    const std::string d = dir.empty() ? findInstalledApp(identity.substr(8)) : dir;
    InstallRecord rec;
    if (d.empty() || !readInstallRecord(d, rec)) return "unrecorded";
    return rec.hash;
}

namespace {

// The record for `identity`, made current: a decision made now is made for
// what is installed now, so decisions made for other content are dropped
// first (they would be at the app's next launch anyway).
AppGrantRecord& currentRecord(GrantStoreData& data, const std::string& identity, const std::string& dir) {
    AppGrantRecord& rec = data.apps[identity];
    if (rec.dir.empty()) rec.dir = dir;
    // A folder's identity is its folder.
    if (rec.dir.empty() && identity.rfind("path:", 0) == 0) rec.dir = identity.substr(5);
    if (rec.dir.empty() && identity.rfind("install:", 0) == 0) rec.dir = findInstalledApp(identity.substr(8));
    if (rec.id.empty() && !rec.dir.empty()) {
        AppPermissionsEntry e;
        readDeclared(rec.dir, e);
        rec.id = e.id;
    }
    const std::string hash = currentInstallHash(identity, rec.dir);
    if (rec.installHash != hash) {
        rec.decisions.clear();
        rec.picked.clear();
        rec.installHash = hash;
    }
    return rec;
}

}  // namespace

bool resetAppPermissions(const std::string& identity, std::string* error) {
    if (identity.empty()) {
        if (error) *error = "no app to reset";
        return false;
    }
    return updateGrantStore([&](GrantStoreData& data) {
        AppGrantRecord& rec = currentRecord(data, identity, {});
        rec.decisions.clear();
        rec.picked.clear();
    }, error);
}

bool removeAppPicked(const std::string& identity, const std::string& path, std::string* error) {
    bool found = false;
    const bool ok = updateGrantStore([&](GrantStoreData& data) {
        auto it = data.apps.find(identity);
        if (it == data.apps.end()) return;
        auto& picked = it->second.picked;
        auto p = std::find(picked.begin(), picked.end(), path);
        if (p == picked.end()) return;
        picked.erase(p);
        found = true;
    }, error);
    if (ok && !found) {
        if (error) *error = "'" + path + "' is not among what was picked for " + identity;
        return false;
    }
    return ok;
}

bool decideAppCapability(const std::string& identity, const std::string& dir, const std::string& cap,
                         Decision d, std::string* error) {
    const Capability* c = findCapability(cap);
    if (!c) {
        if (error) *error = "\"" + cap + "\" is not a capability (they are: " + capabilityNameList() + ")";
        return false;
    }
    if (c->kind != CapabilityKind::Ask) {
        if (error) *error = "\"" + cap + "\" is granted only by the host executable, never by the grant store";
        return false;
    }
    if (identity.empty()) {
        if (error) *error = "no app to decide for";
        return false;
    }
    return updateGrantStore([&](GrantStoreData& data) {
        AppGrantRecord& rec = currentRecord(data, identity, dir);
        if (d == Decision::Undecided) rec.decisions.erase(cap);
        else rec.decisions[cap] = d;
    }, error);
}

bool setGrantStoreDevMode(bool on, std::string* error) {
    return updateGrantStore([on](GrantStoreData& data) { data.devMode = on; }, error);
}

}  // namespace bro::engine
