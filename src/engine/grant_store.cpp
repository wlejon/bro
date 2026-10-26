#include "engine/grant_store.h"

#include "engine/app_manifest.h"
#include "engine/capabilities.h"
#include "util/json_in.h"
#include "util/json_out.h"
#include "util/log.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

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

std::string grantStoreDir() {
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

std::string grantStorePath() { return u8str(u8path(grantStoreDir()) / "grants.json"); }

GrantStoreData loadGrantStore() {
    GrantStoreData out;
    const std::string path = grantStorePath();
    std::ifstream file(u8path(path), std::ios::binary);
    if (!file.is_open()) return out;
    std::ostringstream ss;
    ss << file.rdbuf();
    util::JsonValue root;
    std::string err;
    if (!util::parseJson(ss.str(), root, &err) || !root.isObject()) {
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

bool updateGrantStore(const std::function<void(GrantStoreData&)>& change, std::string* error) {
    GrantStoreData data = loadGrantStore();
    change(data);
    const fs::path path = u8path(grantStorePath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    // Written beside the store and renamed over it, so a reader in another
    // bro process sees the old store or the new one, never half of one.
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    const fs::path tmp = path.parent_path() / u8path("grants.json." + std::to_string(pid) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (error) *error = "cannot write " + u8str(tmp);
            return false;
        }
        out << serialize(data);
        if (!out.good()) {
            if (error) *error = "cannot write " + u8str(tmp);
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (error) *error = "cannot replace " + u8str(path) + ": " + ec.message();
        return false;
    }
    return true;
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
    std::vector<AppPermissionsEntry> out;
    for (auto& [identity, e] : byIdentity) {
        readDeclared(e.dir, e);
        out.push_back(std::move(e));
    }
    return out;
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
        if (d == Decision::Undecided) rec.decisions.erase(cap);
        else rec.decisions[cap] = d;
    }, error);
}

bool setGrantStoreDevMode(bool on, std::string* error) {
    return updateGrantStore([on](GrantStoreData& data) { data.devMode = on; }, error);
}

}  // namespace bro::engine
