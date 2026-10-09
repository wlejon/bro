#include "engine/app_manifest.h"

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

// ---- a small JSON reader -----------------------------------------------------
// bro.json is read before any JS realm exists, so the engine reads it itself.
// Enough of RFC 8259 for a manifest: objects, arrays, strings (with \uXXXX and
// surrogate pairs), numbers, true/false/null. Comments are not JSON and are
// refused like any other syntax error.

struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* get(const char* key) const {
        if (kind != Object) return nullptr;
        for (const auto& [k, v] : members)
            if (k == key) return &v;
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(const std::string& text) : t_(text) {}

    bool parse(Json& out, std::string& error) {
        skipWs();
        if (!value(out, 0)) { error = err_; return false; }
        skipWs();
        if (p_ != t_.size()) { error = at("trailing characters"); return false; }
        return true;
    }

private:
    const std::string& t_;
    size_t p_ = 0;
    std::string err_;

    std::string at(const char* what) {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < p_ && i < t_.size(); ++i) {
            if (t_[i] == '\n') { ++line; col = 1; } else ++col;
        }
        return std::string(what) + " at line " + std::to_string(line) + ", column " + std::to_string(col);
    }
    bool fail(const char* what) { if (err_.empty()) err_ = at(what); return false; }

    void skipWs() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) ++p_;
    }
    bool lit(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (t_.compare(p_, n, word) != 0) return false;
        p_ += n;
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (p_ >= t_.size()) return fail("unexpected end");
        char c = t_[p_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') { out.kind = Json::String; return string(out.s); }
        if (lit("true")) { out.kind = Json::Bool; out.b = true; return true; }
        if (lit("false")) { out.kind = Json::Bool; out.b = false; return true; }
        if (lit("null")) { out.kind = Json::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool number(Json& out) {
        size_t start = p_;
        if (t_[p_] == '-') ++p_;
        while (p_ < t_.size() && ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' ||
                                  t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-'))
            ++p_;
        std::string num = t_.substr(start, p_ - start);
        char* end = nullptr;
        out.kind = Json::Number;
        out.n = std::strtod(num.c_str(), &end);
        if (!end || *end != '\0') return fail("bad number");
        return true;
    }

    static void putUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) s += char(cp);
        else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F));
        } else {
            s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& v) {
        if (p_ + 4 > t_.size()) return fail("short \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char h = t_[p_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') v |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= uint32_t(h - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++p_;  // opening quote
        while (p_ < t_.size()) {
            char c = t_[p_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (p_ >= t_.size()) break;
            char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 1 < t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u') {
                        p_ += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Json& out, int depth) {
        out.kind = Json::Array;
        ++p_;
        skipWs();
        if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
        for (;;) {
            Json item;
            skipWs();
            if (!value(item, depth + 1)) return false;
            out.items.push_back(std::move(item));
            skipWs();
            if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
            if (p_ < t_.size() && t_[p_] == ']') { ++p_; return true; }
            return fail("expected , or ]");
        }
    }

    bool object(Json& out, int depth) {
        out.kind = Json::Object;
        ++p_;
        skipWs();
        if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
        for (;;) {
            skipWs();
            if (p_ >= t_.size() || t_[p_] != '"') return fail("expected a key");
            std::string key;
            if (!string(key)) return false;
            skipWs();
            if (p_ >= t_.size() || t_[p_] != ':') return fail("expected :");
            ++p_;
            skipWs();
            Json v;
            if (!value(v, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(v));
            skipWs();
            if (p_ < t_.size() && t_[p_] == ',') { ++p_; continue; }
            if (p_ < t_.size() && t_[p_] == '}') { ++p_; return true; }
            return fail("expected , or }");
        }
    }
};

// ---- typed reads ---------------------------------------------------------------

struct Fields {
    const Json& obj;
    const std::string& where;

    void warn(const char* key, const char* want) const {
        LOG_WARN("%s: \"%s\" should be %s; ignored", where.c_str(), key, want);
    }
    void str(const char* key, std::string& out) const {
        const Json* v = obj.get(key);
        if (!v) return;
        if (v->kind == Json::String) out = v->s;
        else warn(key, "a string");
    }
    void boolean(const char* key, bool& out) const {
        const Json* v = obj.get(key);
        if (!v) return;
        if (v->kind == Json::Bool) out = v->b;
        else warn(key, "true or false");
    }
    // A list of strings; a single string reads as a list of one.
    void strings(const char* key, std::vector<std::string>& out) const {
        const Json* v = obj.get(key);
        if (!v) return;
        if (v->kind == Json::String) { out.push_back(v->s); return; }
        if (v->kind != Json::Array) { warn(key, "a list of strings"); return; }
        for (const Json& item : v->items) {
            if (item.kind == Json::String) out.push_back(item.s);
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

    Json root;
    std::string err;
    if (!Reader(text).parse(root, err)) {
        if (error) *error = path + ": " + err;
        return false;
    }
    if (root.kind != Json::Object) {
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
    f.boolean("shell", out.shell);
    {
        std::vector<std::string> perms;
        f.strings("permissions", perms);
        f.strings("privileged", perms);
        for (const auto& p : perms) addUnique(out.permissions, p);
    }

    if (const Json* ft = root.get("fileTypes")) {
        if (ft->kind != Json::Array) f.warn("fileTypes", "a list of {name, mimeTypes, extensions}");
        else for (const Json& item : ft->items) {
            if (item.kind != Json::Object) { f.warn("fileTypes", "a list of objects"); continue; }
            Fields g{item, path};
            AppFileType t;
            g.str("name", t.name);
            g.strings("mimeTypes", t.mimeTypes);
            g.strings("extensions", t.extensions);
            for (auto& e : t.extensions) if (!e.empty() && e[0] == '.') e.erase(0, 1);
            if (!t.mimeTypes.empty() || !t.extensions.empty()) out.fileTypes.push_back(std::move(t));
        }
    }

    if (const Json* acts = root.get("actions")) {
        if (acts->kind != Json::Array) f.warn("actions", "a list of {id, name, args}");
        else for (const Json& item : acts->items) {
            if (item.kind != Json::Object) { f.warn("actions", "a list of objects"); continue; }
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

bool isValidAppId(const std::string& id) {
    if (id.empty() || id.size() > 255) return false;
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
    d.logFile = u8str(u8path(d.logDir) / u8path(id + ".log"));
    return d;
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

std::string userPermissionsFile() {
#if defined(_WIN32)
    std::string base = getEnv("APPDATA");
    if (base.empty()) return {};
    return u8str(u8path(base) / "bro" / "permissions.json");
#elif defined(__APPLE__)
    std::string h = getEnv("HOME");
    if (h.empty()) return {};
    return u8str(u8path(h) / "Library" / "Application Support" / "bro" / "permissions.json");
#else
    std::string base = xdgDir("XDG_CONFIG_HOME", ".config");
    if (base.empty()) return {};
    return u8str(u8path(base) / "bro" / "permissions.json");
#endif
}

std::vector<std::string> userPermissionGrants(const std::string& id) {
    std::vector<std::string> out;
    const std::string path = userPermissionsFile();
    if (path.empty() || id.empty()) return out;
    std::ifstream file(u8path(path), std::ios::binary);
    if (!file.is_open()) return out;
    std::ostringstream ss;
    ss << file.rdbuf();
    Json root;
    std::string err;
    if (!Reader(ss.str()).parse(root, err) || root.kind != Json::Object) {
        LOG_WARN("%s: %s; no permissions granted from it", path.c_str(), err.empty() ? "not a JSON object" : err.c_str());
        return out;
    }
    Fields f{root, path};
    f.strings(id.c_str(), out);
    return out;
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
