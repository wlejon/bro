#include "engine/app_manifest.h"

#include "engine/capabilities.h"
#include "util/json_in.h"
#include "util/log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

// bro.json is read before any JS realm exists, so the engine reads it itself
// (util/json_in.h).
using util::JsonValue;
using JType = JsonValue::Type;

// ---- typed reads ---------------------------------------------------------------

struct Fields {
    const JsonValue& obj;
    const std::string& where;

    void warn(const char* key, const char* want) const {
        LOG_WARN("%s: \"%s\" should be %s; ignored", where.c_str(), key, want);
    }
    void str(const char* key, std::string& out) const {
        const JsonValue* v = obj.find(key);
        if (!v) return;
        if (v->type == JType::String) out = v->string;
        else warn(key, "a string");
    }
    void boolean(const char* key, bool& out) const {
        const JsonValue* v = obj.find(key);
        if (!v) return;
        if (v->type == JType::Bool) out = v->boolean;
        else warn(key, "true or false");
    }
    // A list of strings; a single string reads as a list of one.
    void strings(const char* key, std::vector<std::string>& out) const {
        const JsonValue* v = obj.find(key);
        if (!v) return;
        if (v->type == JType::String) { out.push_back(v->string); return; }
        if (v->type != JType::Array) { warn(key, "a list of strings"); return; }
        for (const JsonValue& item : v->array) {
            if (item.type == JType::String) out.push_back(item.string);
            else { warn(key, "a list of strings"); return; }
        }
    }
};

void addUnique(std::vector<std::string>& list, const std::string& s) {
    for (const auto& x : list) if (x == s) return;
    list.push_back(s);
}

std::string getEnv(const char* name) {
#ifdef _WIN32
    // The wide API: %APPDATA% holds the user's name, in any script.
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring buf(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    buf.resize(n);
    if (buf.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), int(buf.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), int(buf.size()), out.data(), len, nullptr, nullptr);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

// An XDG base directory: the variable when it holds an absolute path, else
// $HOME/<fallback>. "" when neither is known.
std::string xdgDir(const char* var, const char* homeRelative) {
    std::string v = getEnv(var);
    if (!v.empty() && u8path(v).is_absolute()) return v;
    std::string home = getEnv("HOME");
    return home.empty() ? std::string() : u8str(u8path(home) / homeRelative);
}

}  // namespace

bool parseAppManifest(const std::string& path, AppDescriptor& out, std::string* error) {
    std::ifstream file(u8path(path), std::ios::binary);
    if (!file.is_open()) {
        if (error) *error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);  // a BOM, as some Windows editors write

    JsonValue root;
    std::string err;
    if (!util::parseJson(text, root, &err)) {
        if (error) *error = path + ": " + err;
        return false;
    }
    if (!root.isObject()) {
        if (error) *error = path + ": not a JSON object";
        return false;
    }

    Fields f{root, path};
    f.str("id", out.id);
    if (!out.id.empty() && !isValidAppId(out.id)) {
        LOG_WARN("%s: \"id\" \"%s\" is not a valid app id ([A-Za-z0-9._-], reverse-DNS or a slug); "
                 "the folder name is used instead", path.c_str(), out.id.c_str());
        out.id.clear();
    }
    f.str("name", out.name);
    f.str("version", out.version);
    f.str("icon", out.icon);
    f.str("description", out.description);
    f.strings("categories", out.categories);
    f.strings("keywords", out.keywords);
    f.boolean("singleInstance", out.singleInstance);
    f.boolean("display", out.display);
    {
        // Unlike the other keys, a mistake here fails the manifest: an app
        // that names a capability bro does not have would otherwise run
        // without it and fail somewhere far from the typo.
        std::vector<std::string> perms;
        if (const JsonValue* v = root.find("permissions"); v && !(v->isArray() || v->isString())) {
            if (error) *error = path + ": \"permissions\" should be a list of capability names";
            return false;
        }
        f.strings("permissions", perms);
        for (const auto& p : perms) {
            if (!findCapability(p)) {
                if (error)
                    *error = path + ": \"permissions\" names \"" + p + "\", which is not a capability (they are: " +
                             capabilityNameList() + "; docs/apps.md, Permissions)";
                return false;
            }
            addUnique(out.permissions, p);
        }
    }

    if (const JsonValue* ft = root.find("fileTypes")) {
        if (!ft->isArray()) f.warn("fileTypes", "a list of {name, mimeTypes, extensions}");
        else for (const JsonValue& item : ft->array) {
            if (!item.isObject()) { f.warn("fileTypes", "a list of objects"); continue; }
            Fields g{item, path};
            AppFileType t;
            g.str("name", t.name);
            g.strings("mimeTypes", t.mimeTypes);
            g.strings("extensions", t.extensions);
            for (auto& e : t.extensions) if (!e.empty() && e[0] == '.') e.erase(0, 1);
            if (!t.mimeTypes.empty() || !t.extensions.empty()) out.fileTypes.push_back(std::move(t));
        }
    }

    if (const JsonValue* acts = root.find("actions")) {
        if (!acts->isArray()) f.warn("actions", "a list of {id, name, args}");
        else for (const JsonValue& item : acts->array) {
            if (!item.isObject()) { f.warn("actions", "a list of objects"); continue; }
            Fields g{item, path};
            AppAction a;
            g.str("id", a.id);
            g.str("name", a.name);
            g.strings("args", a.args);
            g.str("icon", a.icon);
            bool okId = !a.id.empty();
            for (char c : a.id)
                if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                    okId = false;
            if (!okId || a.name.empty()) {
                LOG_WARN("%s: an action needs an \"id\" of [A-Za-z0-9-] and a \"name\"; skipped", path.c_str());
                continue;
            }
            out.actions.push_back(std::move(a));
        }
    }
    return true;
}

bool isReservedAppId(const std::string& id) {
    // bro's own config directory (grant_store.h, the grant store) is where
    // an app called "bro" would keep its config: no app may be called that.
    // Case-blind, as the file systems that hold it can be.
    if (id.size() != 3) return false;
    auto lower = [](char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    return lower(id[0]) == 'b' && lower(id[1]) == 'r' && lower(id[2]) == 'o';
}

bool isValidAppId(const std::string& id) {
    if (id.empty() || id.size() > 255) return false;
    if (isReservedAppId(id)) return false;
    if (id.front() == '.' || id.back() == '.' || id.find("..") != std::string::npos) return false;
    for (unsigned char c : id) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string appIdFor(const AppDescriptor& manifest, const std::string& appDir) {
    if (!manifest.id.empty() && isValidAppId(manifest.id)) return manifest.id;
    if (appDir.empty()) return {};
    std::error_code ec;
    fs::path abs = fs::absolute(u8path(appDir), ec);
    if (ec) abs = u8path(appDir);
    std::string s = u8str(abs.lexically_normal());
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    std::string name = u8str(u8path(s).filename());
    std::string slug;
    for (unsigned char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        slug += ok ? static_cast<char>(c) : '_';
    }
    while (!slug.empty() && slug.front() == '.') slug.erase(slug.begin());
    while (!slug.empty() && slug.back() == '.') slug.pop_back();
    while (slug.find("..") != std::string::npos) slug.replace(slug.find(".."), 2, "_");
    if (isReservedAppId(slug)) slug = "app." + slug;
    return slug.empty() ? std::string("app") : slug;
}

AppDirs appDirsFor(const std::string& idIn) {
    const std::string id = idIn.empty() ? std::string("app") : idIn;
    AppDirs d;
    std::string home = getEnv("BRO_APP_HOME");
    if (!home.empty()) {
        fs::path h = u8path(home);
        std::error_code ec;
        fs::path abs = fs::absolute(h, ec);
        if (!ec) h = abs;
        d.config = u8str(h / "config");
        d.data = u8str(h / "data");
        d.cache = u8str(h / "cache");
        d.logDir = u8str(h / "log");
    } else {
#if defined(_WIN32)
        std::string roaming = getEnv("APPDATA");
        std::string local = getEnv("LOCALAPPDATA");
        std::string profile = getEnv("USERPROFILE");
        if (roaming.empty() && !profile.empty()) roaming = u8str(u8path(profile) / "AppData" / "Roaming");
        if (local.empty() && !profile.empty()) local = u8str(u8path(profile) / "AppData" / "Local");
        if (roaming.empty()) roaming = ".bro";
        if (local.empty()) local = roaming;
        d.config = u8str(u8path(roaming) / u8path(id));
        d.data = u8str(u8path(roaming) / u8path(id) / "data");
        d.cache = u8str(u8path(local) / u8path(id) / "cache");
        d.logDir = u8str(u8path(local) / u8path(id));
#elif defined(__APPLE__)
        std::string h = getEnv("HOME");
        fs::path lib = u8path(h.empty() ? std::string(".bro") : h) / "Library";
        d.config = u8str(lib / "Application Support" / u8path(id));
        d.data = u8str(lib / "Application Support" / u8path(id) / "data");
        d.cache = u8str(lib / "Caches" / u8path(id));
        d.logDir = u8str(lib / "Logs" / u8path(id));
#else
        auto under = [&](const char* var, const char* rel) {
            std::string base = xdgDir(var, rel);
            return u8str(u8path(base.empty() ? std::string(".bro") : base) / u8path(id));
        };
        d.config = under("XDG_CONFIG_HOME", ".config");
        d.data = under("XDG_DATA_HOME", ".local/share");
        d.cache = under("XDG_CACHE_HOME", ".cache");
        d.logDir = under("XDG_STATE_HOME", ".local/state");
#endif
    }
    // Normal form whatever the environment held (BRO_APP_HOME or a TMPDIR
    // ending in a separator, '..', doubled separators): script joins onto
    // these and compares (path.join normalizes), so a '.../T//x/data' would
    // not be the start of a file joined under it.
    auto normal = [](std::string& s) {
        if (s.empty()) return;
        s = u8str(u8path(s).lexically_normal());
        while (s.size() > 1 && (s.back() == '/' || s.back() == '\\') &&
               !(s.size() == 3 && s[1] == ':'))
            s.pop_back();
    };
    normal(d.config);
    normal(d.data);
    normal(d.cache);
    normal(d.logDir);
    d.logFile = u8str(u8path(d.logDir) / u8path(id + ".log"));
    return d;
}

std::string installRoot(bool system) {
#ifdef _WIN32
    const std::string pf = getEnv("ProgramFiles");
    if (system) return u8str(u8path(pf.empty() ? std::string("C:/Program Files") : pf) / "bro" / "apps");
    std::string local = getEnv("LOCALAPPDATA");
    if (local.empty()) local = getEnv("APPDATA");
    if (local.empty()) local = ".";
    return u8str(u8path(local) / "bro" / "apps");
#elif defined(__APPLE__)
    if (system) return "/Library/Application Support/bro/apps";
    const std::string h = getEnv("HOME");
    return u8str(u8path(h.empty() ? std::string(".") : h) / "Library" / "Application Support" / "bro" / "apps");
#else
    if (system) return "/usr/local/share/bro/apps";
    std::string dataHome = getEnv("XDG_DATA_HOME");
    if (dataHome.empty() || dataHome[0] != '/') {
        const std::string h = getEnv("HOME");
        dataHome = u8str(u8path(h.empty() ? std::string(".") : h) / ".local" / "share");
    }
    return u8str(u8path(dataHome) / "bro" / "apps");
#endif
}

std::vector<std::string> installedAppRoots() {
    std::vector<std::string> roots;
#if defined(_WIN32)
    std::string local = getEnv("LOCALAPPDATA");
    if (!local.empty()) roots.push_back(u8str(u8path(local) / "bro" / "apps"));
    std::string pf = getEnv("ProgramFiles");
    if (!pf.empty()) roots.push_back(u8str(u8path(pf) / "bro" / "apps"));
#else
#if defined(__APPLE__)
    std::string h = getEnv("HOME");
    if (!h.empty()) roots.push_back(u8str(u8path(h) / "Library" / "Application Support" / "bro" / "apps"));
    roots.push_back("/Library/Application Support/bro/apps");
#endif
    std::string dataHome = xdgDir("XDG_DATA_HOME", ".local/share");
    if (!dataHome.empty()) roots.push_back(u8str(u8path(dataHome) / "bro" / "apps"));
    std::string dirs = getEnv("XDG_DATA_DIRS");
    if (dirs.empty()) dirs = "/usr/local/share:/usr/share";
    std::stringstream ss(dirs);
    std::string item;
    while (std::getline(ss, item, ':')) {
        if (!item.empty() && item[0] == '/') roots.push_back(u8str(u8path(item) / "bro" / "apps"));
    }
#endif
    return roots;
}

std::string findInstalledApp(const std::string& id) {
    if (!isValidAppId(id)) return {};
    for (const std::string& root : installedAppRoots()) {
        fs::path dir = u8path(root) / u8path(id);
        std::error_code ec;
        if (fs::is_directory(dir, ec) &&
            (fs::exists(dir / "bro.json", ec) || fs::exists(dir / "index.html", ec)))
            return u8str(dir);
    }
    return {};
}

}  // namespace bro::engine
