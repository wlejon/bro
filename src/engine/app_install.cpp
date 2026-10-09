// `bro --install` and friends: folder apps as installed desktop applications
// (docs/apps.md).
//
//   bro --install <dir> [--link] [--system] [--exec <bro>]
//       copy the app (or with --link, symlink it) to <root>/<id> and write its
//       desktop entry, <applications>/<id>.desktop, so launchers (bro.apps,
//       helm, any FreeDesktop menu) list it with no special case
//   bro --uninstall <id> [--system]
//   bro --list-apps
//   bro --desktop-entry <dir> [--exec <bro>]   print the entry, write nothing
//
// The roots are app_manifest.h's installedAppRoots(): the user's
// ($XDG_DATA_HOME/bro/apps, %LOCALAPPDATA%\bro\apps, ~/Library/Application
// Support/bro/apps) unless --system (/usr/local/share/bro/apps,
// %ProgramFiles%\bro\apps, /Library/Application Support/bro/apps). Desktop entries go to
// $XDG_DATA_HOME/applications (--system: /usr/local/share/applications);
// Windows and macOS keep the copy and skip the entry.

#include "engine/app_manifest.h"
#include "engine/launcher.h"
#include "util/exe_dir.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

// A GUI-subsystem bro.exe started from a console has no stdout of its own;
// borrow the parent console's (output already going to a pipe or a file keeps
// it).
void attachConsole() {
#ifdef _WIN32
    auto redirected = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        if (!h || h == INVALID_HANDLE_VALUE) return false;
        DWORD t = GetFileType(h);
        return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
    };
    bool out = redirected(STD_OUTPUT_HANDLE), err = redirected(STD_ERROR_HANDLE);
    if ((!out || !err) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        if (!out) freopen_s(&f, "CONOUT$", "w", stdout);
        if (!err) freopen_s(&f, "CONOUT$", "w", stderr);
    }
#endif
}

// ---- desktop entry ------------------------------------------------------------

// A string value: the spec's escapes for \, newline, tab and CR.
std::string deString(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

// A list value (Categories=, Keywords=, MimeType=): ';'-terminated, ';' in an
// item escaped.
std::string deList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& i : items) {
        if (i.empty()) continue;
        for (char c : i) {
            if (c == ';') out += "\\;";
            else out += c;
        }
        out += ';';
    }
    return deString(out);
}

// One Exec argument. Quoted when it holds anything the spec reserves; inside
// quotes ", `, $ and \ take a backslash, and '%' is doubled everywhere so no
// argument reads as a field code. The line is then a string value, so its
// backslashes are escaped once more (deString).
std::string execArg(const std::string& a) {
    std::string esc;
    bool quote = a.empty();
    for (char c : a) {
        if (c == '%') { esc += "%%"; continue; }
        if (c == '"' || c == '`' || c == '$' || c == '\\') { esc += '\\'; quote = true; }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\'' || c == '>' || c == '<' || c == '~' ||
            c == '|' || c == '&' || c == ';' || c == '*' || c == '?' || c == '#' || c == '(' || c == ')')
            quote = true;
        esc += c;
    }
    return quote ? "\"" + esc + "\"" : esc;
}

std::string execLine(const std::vector<std::string>& argv, const char* fieldCode) {
    std::string line;
    for (const auto& a : argv) {
        if (!line.empty()) line += ' ';
        line += execArg(a);
    }
    if (fieldCode) { line += ' '; line += fieldCode; }
    return deString(line);
}

std::string defaultExec() {
#ifdef _WIN32
    return u8str(u8path(util::executableDir()) / "bro.exe");
#else
    return u8str(u8path(util::executableDir()) / "bro");
#endif
}

struct Entry {
    std::string id;
    std::string text;
};

bool readManifest(const std::string& dir, AppDescriptor& m, std::string& err) {
    fs::path json = u8path(dir) / "bro.json";
    std::error_code ec;
    if (!fs::exists(json, ec)) {
        if (fs::exists(u8path(dir) / "index.html", ec)) return true;  // a bare page: defaults
        err = dir + " is not an app (no bro.json or index.html)";
        return false;
    }
    return parseAppManifest(u8str(json), m, &err);
}

Entry desktopEntry(const std::string& appDir, const AppDescriptor& m, const std::string& exec) {
    Entry e;
    e.id = appIdFor(m, appDir);
    std::ostringstream o;
    o << "[Desktop Entry]\n";
    o << "Type=Application\n";
    o << "Version=1.5\n";
    o << "Name=" << deString(m.name.empty() ? e.id : m.name) << "\n";
    if (!m.description.empty()) o << "Comment=" << deString(m.description) << "\n";
    if (!m.icon.empty()) {
        std::error_code ec;
        fs::path icon = u8path(appDir) / u8path(m.icon);
        o << "Icon=" << deString(u8str(fs::exists(icon, ec) ? icon : u8path(m.icon))) << "\n";
    }
    // Files are handed to the app as arguments: %F when it opens any.
    std::vector<std::string> mimes;
    for (const auto& t : m.fileTypes)
        for (const auto& mt : t.mimeTypes) mimes.push_back(mt);
    o << "Exec=" << execLine({exec, appDir}, m.fileTypes.empty() ? nullptr : "%F") << "\n";
    o << "TryExec=" << deString(exec) << "\n";
    o << "Terminal=false\n";
    o << "StartupNotify=true\n";
    o << "StartupWMClass=" << deString(e.id) << "\n";
    if (!m.categories.empty()) o << "Categories=" << deList(m.categories) << "\n";
    if (!m.keywords.empty()) o << "Keywords=" << deList(m.keywords) << "\n";
    if (!mimes.empty()) o << "MimeType=" << deList(mimes) << "\n";
    if (!m.display) o << "NoDisplay=true\n";
    if (m.singleInstance) o << "SingleMainWindow=true\n";
    if (!m.version.empty()) o << "X-Bro-Version=" << deString(m.version) << "\n";
    o << "X-Bro-App=" << deString(appDir) << "\n";
    if (!m.actions.empty()) {
        std::vector<std::string> ids;
        for (const auto& a : m.actions) ids.push_back(a.id);
        o << "Actions=" << deList(ids) << "\n";
    }
    for (const auto& a : m.actions) {
        o << "\n[Desktop Action " << a.id << "]\n";
        o << "Name=" << deString(a.name) << "\n";
        std::vector<std::string> argv{exec, appDir};
        argv.insert(argv.end(), a.args.begin(), a.args.end());
        o << "Exec=" << execLine(argv, nullptr) << "\n";
        if (!a.icon.empty()) o << "Icon=" << deString(a.icon) << "\n";
    }
    e.text = o.str();
    return e;
}

// ---- roots ------------------------------------------------------------------------

std::string appsRoot(bool system) {
#ifdef _WIN32
    if (system) return u8str(u8path(envOr("ProgramFiles", "C:/Program Files")) / "bro" / "apps");
    return u8str(u8path(envOr("LOCALAPPDATA", envOr("APPDATA", "."))) / "bro" / "apps");
#elif defined(__APPLE__)
    // installedAppRoots()' macOS roots: the user's Library, then the machine's.
    if (system) return "/Library/Application Support/bro/apps";
    return u8str(u8path(envOr("HOME", ".")) / "Library" / "Application Support" / "bro" / "apps");
#else
    if (system) return "/usr/local/share/bro/apps";
    std::string dataHome = envOr("XDG_DATA_HOME", "");
    if (dataHome.empty() || dataHome[0] != '/') dataHome = u8str(u8path(envOr("HOME", ".")) / ".local" / "share");
    return u8str(u8path(dataHome) / "bro" / "apps");
#endif
}

// "" where this platform has no desktop entries.
std::string applicationsDir(bool system) {
#if defined(_WIN32) || defined(__APPLE__)
    (void)system;
    return {};
#else
    if (system) return "/usr/local/share/applications";
    std::string dataHome = envOr("XDG_DATA_HOME", "");
    if (dataHome.empty() || dataHome[0] != '/') dataHome = u8str(u8path(envOr("HOME", ".")) / ".local" / "share");
    return u8str(u8path(dataHome) / "applications");
#endif
}

bool writeFileAtomic(const fs::path& path, const std::string& text, std::string& err) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + u8str(tmp); return false; }
        f << text;
        if (!f) { err = "cannot write " + u8str(tmp); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) { err = "cannot replace " + u8str(path) + ": " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

void removeAll(const fs::path& p) {
    std::error_code ec;
    if (fs::is_symlink(fs::symlink_status(p, ec))) fs::remove(p, ec);
    else fs::remove_all(p, ec);
}

bool copyApp(const fs::path& from, const fs::path& to, std::string& err) {
    std::error_code ec;
    fs::path staging = to;
    staging += ".new";
    removeAll(staging);
    fs::create_directories(staging, ec);
    if (ec) { err = "cannot create " + u8str(staging) + ": " + ec.message(); return false; }
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path rel = fs::relative(it->path(), from, ec);
        const std::string first = u8str(*rel.begin());
        // Version control and the app's own build output are not the app.
        if (first == ".git" || first == ".github" || first == "node_modules") {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        const fs::path dst = staging / rel;
        if (it->is_directory(ec)) fs::create_directories(dst, ec);
        else fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { err = "cannot copy " + u8str(it->path()) + ": " + ec.message(); removeAll(staging); return false; }
    }
    if (ec) { err = "cannot read " + u8str(from) + ": " + ec.message(); removeAll(staging); return false; }
    fs::path old = to;
    old += ".old";
    removeAll(old);
    if (fs::exists(fs::symlink_status(to, ec))) fs::rename(to, old, ec);
    fs::rename(staging, to, ec);
    if (ec) { err = "cannot move the copy into " + u8str(to) + ": " + ec.message(); return false; }
    removeAll(old);
    return true;
}

int usage() {
    std::fprintf(stderr,
        "usage: bro --install <app-dir> [--link] [--system] [--exec <bro>]\n"
        "       bro --uninstall <id> [--system]\n"
        "       bro --list-apps\n"
        "       bro --desktop-entry <app-dir> [--exec <bro>]\n");
    return 2;
}

}  // namespace

int runAppCommand(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    const std::string& cmd = args[0];
    if (cmd != "--install" && cmd != "--uninstall" && cmd != "--list-apps" && cmd != "--desktop-entry") return -1;
    attachConsole();

    bool link = false, system = false;
    std::string exec, target;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--link") link = true;
        else if (args[i] == "--system") system = true;
        else if (args[i] == "--exec" && i + 1 < args.size()) exec = args[++i];
        else if (target.empty()) target = args[i];
        else return usage();
    }
    if (exec.empty()) exec = defaultExec();

    if (cmd == "--list-apps") {
        for (const std::string& root : installedAppRoots()) {
            std::error_code ec;
            for (auto& e : fs::directory_iterator(u8path(root), ec)) {
                if (!e.is_directory(ec)) continue;
                const std::string name = u8str(e.path().filename());
                if (!isValidAppId(name) || name.ends_with(".new") || name.ends_with(".old")) continue;
                std::printf("%s\t%s\n", name.c_str(), u8str(e.path()).c_str());
            }
        }
        return 0;
    }

    if (target.empty()) return usage();

    if (cmd == "--uninstall") {
        if (!isValidAppId(target)) { std::fprintf(stderr, "bro: '%s' is not an app id\n", target.c_str()); return 1; }
        bool any = false;
        fs::path dir = u8path(appsRoot(system)) / u8path(target);
        std::error_code ec;
        if (fs::exists(fs::symlink_status(dir, ec))) { removeAll(dir); any = true; }
        const std::string apps = applicationsDir(system);
        if (!apps.empty()) {
            fs::path entry = u8path(apps) / u8path(target + ".desktop");
            if (fs::remove(entry, ec)) any = true;
        }
        if (!any) { std::fprintf(stderr, "bro: %s is not installed in %s\n", target.c_str(), appsRoot(system).c_str()); return 1; }
        std::printf("uninstalled %s\n", target.c_str());
        return 0;
    }

    std::error_code ec;
    fs::path src = fs::absolute(u8path(target), ec);
    src = src.lexically_normal();
    if (!src.has_filename()) src = src.parent_path();
    AppDescriptor m;
    std::string err;
    if (!readManifest(u8str(src), m, err)) { std::fprintf(stderr, "bro: %s\n", err.c_str()); return 1; }
    const std::string id = appIdFor(m, u8str(src));

    if (cmd == "--desktop-entry") {
        std::fputs(desktopEntry(u8str(src), m, exec).text.c_str(), stdout);
        return 0;
    }

    // --install
    fs::path dest = u8path(appsRoot(system)) / u8path(id);
    fs::create_directories(dest.parent_path(), ec);
    if (fs::equivalent(src, dest, ec)) {
        // Already in place (a package dropped it there): only the entry.
    } else if (link) {
        removeAll(dest);
        fs::create_directory_symlink(src, dest, ec);
        if (ec) { std::fprintf(stderr, "bro: cannot link %s: %s\n", u8str(dest).c_str(), ec.message().c_str()); return 1; }
    } else if (!copyApp(src, dest, err)) {
        std::fprintf(stderr, "bro: %s\n", err.c_str());
        return 1;
    }

    const std::string apps = applicationsDir(system);
    if (!apps.empty()) {
        // The entry names the installed path, so it keeps working when the
        // source tree moves (a --link follows the source, by design).
        Entry e = desktopEntry(u8str(dest), m, exec);
        fs::path entryPath = u8path(apps) / u8path(id + ".desktop");
        if (!writeFileAtomic(entryPath, e.text, err)) { std::fprintf(stderr, "bro: %s\n", err.c_str()); return 1; }
        std::printf("installed %s at %s\ndesktop entry %s\n", id.c_str(), u8str(dest).c_str(), u8str(entryPath).c_str());
    } else {
        std::printf("installed %s at %s\n", id.c_str(), u8str(dest).c_str());
    }
    return 0;
}

}  // namespace bro::engine
