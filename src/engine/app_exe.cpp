// An installed app's own executable: the identity block, the cache key, and
// building one from the stub (app_exe.h). Windows' resources and Start menu
// shortcut are app_exe_win.cpp.

#include "engine/app_exe.h"

#include "engine/app_runtime.h"
#include "util/exe_dir.h"
#include "util/process_spawn.h"
#include "util/sha.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef BRO_VERSION_STRING
#define BRO_VERSION_STRING "0"
#endif

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

bool writeFile(const fs::path& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

// The one block an executable holds: its offset in `bytes`, or npos when there
// is none or (a file that is not one of ours) more than one.
size_t findBlock(const std::string& bytes) {
    const std::string_view marker(kAppExeMarker, kAppExeMarkerSize);
    const size_t at = bytes.find(marker);
    if (at == std::string::npos) return at;
    if (bytes.find(marker, at + 1) != std::string::npos) return std::string::npos;
    if (at + kAppExeIdentityCapacity > bytes.size()) return std::string::npos;
    return at;
}

std::string blockText(const std::string& bytes, size_t at) {
    const char* p = bytes.data() + at + kAppExeMarkerSize;
    const size_t cap = kAppExeIdentityCapacity - kAppExeMarkerSize;
    size_t n = 0;
    while (n < cap && p[n] != '\0') ++n;
    return std::string(p, n);
}

std::string engineDir() { return util::engineDir(); }

}  // namespace

// ---- the identity block -------------------------------------------------------

std::string serializeAppExeIdentity(const AppExeIdentity& id) {
    std::string out;
    auto line = [&out](const char* key, const std::string& v) {
        // A value is one line; nothing an id, a version or a path holds here
        // has a newline in it, and one that did would end the value.
        std::string clean;
        for (char c : v) clean += (c == '\n' || c == '\r') ? ' ' : c;
        out += key;
        out += '=';
        out += clean;
        out += '\n';
    };
    line("format", "1");
    line("id", id.id);
    line("name", id.name);
    line("version", id.version);
    line("app", id.app);
    line("engine", id.engine);
    line("hash", id.hash);
    line("libbro", id.libbro);
    line("bro", id.bro);
    return out;
}

bool parseAppExeIdentity(std::string_view text, AppExeIdentity& out) {
    out = AppExeIdentity{};
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view lineV = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = lineV.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = lineV.substr(0, eq);
        const std::string v(lineV.substr(eq + 1));
        if (k == "format") out.format = std::atoi(v.c_str());
        else if (k == "id") out.id = v;
        else if (k == "name") out.name = v;
        else if (k == "version") out.version = v;
        else if (k == "app") out.app = v;
        else if (k == "engine") out.engine = v;
        else if (k == "hash") out.hash = v;
        else if (k == "libbro") out.libbro = v;
        else if (k == "bro") out.bro = v;
        // Keys a later executable carries (aot=, snapshot=, ...) are skipped.
    }
    return out.format == 1 && !out.app.empty();
}

// ---- the engine ------------------------------------------------------------------

std::string engineImagePath() { return util::engineImagePath(); }

const std::string& engineVersion() {
    static const std::string v = [] {
        std::string s = BRO_VERSION_STRING;
        s += "+abi1";
        const std::string image = engineImagePath();
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a{};
        std::u8string u(image.begin(), image.end());
        if (GetFileAttributesExW(fs::path(u).c_str(), GetFileExInfoStandard, &a)) {
            const unsigned long long size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
            const unsigned long long mtime =
                (static_cast<unsigned long long>(a.ftLastWriteTime.dwHighDateTime) << 32) |
                a.ftLastWriteTime.dwLowDateTime;
            s += "." + std::to_string(size) + "." + std::to_string(mtime);
        }
#else
        struct stat st {};
        if (::stat(image.c_str(), &st) == 0) {
#if defined(__APPLE__)
            const long long ns = static_cast<long long>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
            const long long ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
            s += "." + std::to_string(static_cast<long long>(st.st_size)) + "." + std::to_string(ns);
        }
#endif
        return s;
    }();
    return v;
}

std::string stockBroPath() {
#ifdef _WIN32
    return u8str(u8path(engineDir()) / "bro.exe");
#else
    return u8str(u8path(engineDir()) / "bro");
#endif
}

std::string appExeStubPath() {
#ifdef _WIN32
    fs::path p = u8path(engineDir()) / "bro-app-stub.exe";
#else
    fs::path p = u8path(engineDir()) / "bro-app-stub";
#endif
    std::error_code ec;
    return fs::is_regular_file(p, ec) ? u8str(p) : std::string();
}

// ---- naming and place --------------------------------------------------------------

std::string appExeName(const std::string& id) {
    std::string n;
    for (char c : id) n += (c == '.') ? '-' : c;
    if (n.empty()) n = "bro-app";
#ifdef _WIN32
    n += ".exe";
#endif
    return n;
}

std::string appExeDir(bool system) { return u8str(u8path(installRoot(system)).parent_path() / "bin"); }

std::string appExePath(const std::string& id, bool system) {
    return u8str(u8path(appExeDir(system)) / u8path(appExeName(id)));
}

// ---- the cache key -------------------------------------------------------------------

std::string appContentHash(const std::string& appDir, const AppDescriptor& manifest) {
    util::Sha256 h;
    const std::string tag = "bro-app-exe/1\n";
    h.update(tag.data(), tag.size());
    auto add = [&h](const std::string& name, const fs::path& p) {
        std::string bytes;
        const bool have = readFile(p, bytes);
        std::string head = name + '\0' + (have ? std::to_string(bytes.size()) : std::string("-")) + '\0';
        h.update(head.data(), head.size());
        if (have) h.update(bytes.data(), bytes.size());
    };
    const fs::path dir = u8path(appDir);
    add("bro.json", dir / "bro.json");
    if (!manifest.icon.empty()) add("icon:" + manifest.icon, dir / u8path(manifest.icon));
    // Later: the AOT module, the heap snapshot, packed assets, each added here
    // under its own name.
    return h.hex();
}

// ---- reading and judging an executable ----------------------------------------------------

bool readAppExeIdentity(const std::string& exePath, AppExeIdentity& out) {
    std::string bytes;
    if (!readFile(u8path(exePath), bytes)) return false;
    const size_t at = findBlock(bytes);
    if (at == std::string::npos) return false;
    return parseAppExeIdentity(blockText(bytes, at), out);
}

const char* appExeStateName(AppExeState s) {
    switch (s) {
        case AppExeState::Missing: return "missing";
        case AppExeState::Current: return "current";
        case AppExeState::StaleEngine: return "built against another engine";
        case AppExeState::StaleApp: return "the app changed since";
        case AppExeState::Foreign: return "not this app's";
    }
    return "?";
}

AppExeState appExeState(const std::string& exePath, const std::string& appDir, AppExeIdentity* stamped) {
    std::error_code ec;
    if (!fs::is_regular_file(u8path(exePath), ec)) return AppExeState::Missing;
    AppExeIdentity id;
    if (!readAppExeIdentity(exePath, id)) return AppExeState::Foreign;
    if (stamped) *stamped = id;
    if (!fs::equivalent(u8path(id.app), u8path(appDir), ec)) return AppExeState::Foreign;
    if (id.engine != engineVersion()) return AppExeState::StaleEngine;
    AppDescriptor m;
    const fs::path json = u8path(appDir) / "bro.json";
    if (fs::exists(json, ec)) parseAppManifest(u8str(json), m, nullptr);
    if (id.hash != appContentHash(appDir, m)) return AppExeState::StaleApp;
    return AppExeState::Current;
}

std::string currentAppExecutable(const std::string& id, const std::string& dir) {
    if (!isValidAppId(id)) return {};
    for (bool system : {false, true}) {
        const std::string exe = appExePath(id, system);
        if (appExeState(exe, dir) == AppExeState::Current) return exe;
    }
    return {};
}

// ---- building one ----------------------------------------------------------------------------

namespace {

// Files a previous build moved aside on Windows (a running executable cannot
// be replaced, only renamed): <exe>.old-<n>. Removed when nothing runs them.
void removeLeftovers(const fs::path& exe) {
    std::error_code ec;
    const std::string stem = u8str(exe.filename()) + ".old-";
    for (auto it = fs::directory_iterator(exe.parent_path(), ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const std::string n = u8str(it->path().filename());
        if (n.rfind(stem, 0) == 0) {
            std::error_code rec;
            fs::remove(it->path(), rec);  // fails, harmlessly, while that copy runs
        }
    }
}

bool replaceFile(const fs::path& fresh, const fs::path& exe, std::string& err) {
    std::error_code ec;
#ifdef _WIN32
    if (fs::exists(exe, ec)) {
        // A running image refuses MoveFileEx(REPLACE_EXISTING) but allows a
        // rename: move it aside under a unique name, then put the new one in.
        fs::path aside = exe;
        aside += ".old-" + std::to_string(static_cast<long long>(_getpid())) + "-" +
                 std::to_string(static_cast<long long>(GetTickCount64()));
        ec.clear();
        fs::rename(exe, aside, ec);
        if (ec) { err = "cannot move " + u8str(exe) + " aside: " + ec.message(); return false; }
        fs::rename(fresh, exe, ec);
        if (ec) {
            std::error_code back;
            fs::rename(aside, exe, back);
            err = "cannot replace " + u8str(exe) + ": " + ec.message();
            return false;
        }
        std::error_code rec;
        fs::remove(aside, rec);  // gone now unless that copy is running
        return true;
    }
#endif
    fs::rename(fresh, exe, ec);
    if (ec) { err = "cannot replace " + u8str(exe) + ": " + ec.message(); return false; }
    return true;
}

}  // namespace

bool buildAppExe(const std::string& appDir, const AppDescriptor& manifest, const std::string& exePath,
                 bool force, bool* rebuilt, std::string& err, const std::string& stampEngine) {
    if (rebuilt) *rebuilt = false;
    const std::string id = appIdFor(manifest, appDir);
    if (!force && stampEngine.empty() && appExeState(exePath, appDir) == AppExeState::Current) return true;

    const std::string stub = appExeStubPath();
    if (stub.empty()) {
        err = "this bro ships no app stub (bro-app-stub beside " + engineImagePath() + ")";
        return false;
    }
    std::string bytes;
    if (!readFile(u8path(stub), bytes)) { err = "cannot read " + stub; return false; }
    const size_t at = findBlock(bytes);
    if (at == std::string::npos) { err = stub + " holds no identity block"; return false; }

    AppExeIdentity ident;
    ident.format = 1;
    ident.id = id;
    ident.name = manifest.name.empty() ? id : manifest.name;
    ident.version = manifest.version;
    ident.app = appDir;
    ident.engine = stampEngine.empty() ? engineVersion() : stampEngine;
    ident.hash = appContentHash(appDir, manifest);
    ident.libbro = engineImagePath();
    ident.bro = stockBroPath();
    const std::string text = serializeAppExeIdentity(ident);
    if (text.size() + 1 > kAppExeIdentityCapacity - kAppExeMarkerSize) {
        err = "the app's identity does not fit the executable's block";
        return false;
    }
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(at + kAppExeMarkerSize),
              bytes.begin() + static_cast<std::ptrdiff_t>(at + kAppExeIdentityCapacity), '\0');
    std::copy(text.begin(), text.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + kAppExeMarkerSize));

    const fs::path exe = u8path(exePath);
    std::error_code ec;
    fs::create_directories(exe.parent_path(), ec);
    removeLeftovers(exe);
    fs::path fresh = exe;
#ifdef _WIN32
    fresh += ".new-" + std::to_string(static_cast<long long>(_getpid()));
#else
    fresh += ".new-" + std::to_string(static_cast<long long>(getpid()));
#endif
    if (!writeFile(fresh, bytes)) { err = "cannot write " + u8str(fresh); return false; }
#ifndef _WIN32
    ::chmod(fresh.c_str(), 0755);
#endif
    const std::string icon = appIconPath(manifest, appDir);
    if (!stampWindowsResources(u8str(fresh), manifest, id, icon, err)) {
        fs::remove(fresh, ec);
        return false;
    }
    if (!replaceFile(fresh, exe, err)) {
        fs::remove(fresh, ec);
        return false;
    }
    if (rebuilt) *rebuilt = true;
    return true;
}

// ---- the AOT directory -------------------------------------------------------------------------

std::string appExeAotDir(const std::string& exePath) {
    fs::path p = u8path(exePath);
#ifdef _WIN32
    if (p.extension() == ".exe" || p.extension() == ".EXE") p.replace_extension();
#endif
    p += ".aot";
    return u8str(p);
}

bool appExeAotEnabled() {
    const char* v = std::getenv("BRO_APP_AOT");
    return !(v && std::string(v) == "0");
}

namespace {

// aot.txt (bronze_host/app_aot.cpp writes it): key=value lines.
std::vector<std::pair<std::string, std::string>> readAotSummary(const std::string& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    std::string text;
    if (!readFile(u8path(dir) / "aot.txt", text)) return out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq != std::string::npos) out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return out;
}

}  // namespace

bool buildAppExeAot(const std::string& appDir, const std::string& exePath, std::string& summary, std::string& err) {
    summary.clear();
#ifdef _WIN32
    const fs::path headless = u8path(engineDir()) / "bro-headless.exe";
#else
    const fs::path headless = u8path(engineDir()) / "bro-headless";
#endif
    std::error_code ec;
    if (!fs::is_regular_file(headless, ec)) {
        err = "this bro ships no bro-headless (" + u8str(headless) + ") to compile with";
        return false;
    }
    const std::string dir = appExeAotDir(exePath);
    fs::create_directories(u8path(dir), ec);
    const std::string log = u8str(u8path(dir) / "build.log");
    const int status = util::runAndWait({u8str(headless), appDir, "--aot-build", dir}, err, log);
    if (status < 0) return false;

    int units = 0, built = 0, current = 0, failed = 0;
    for (const auto& [k, v] : readAotSummary(dir)) {
        if (k == "units") units = std::atoi(v.c_str());
        else if (k == "built") built = std::atoi(v.c_str());
        else if (k == "current") current = std::atoi(v.c_str());
        else if (k.rfind("unit", 0) == 0 && v.find(" failed: ") != std::string::npos) ++failed;
    }
    summary = std::to_string(units) + (units == 1 ? " unit: " : " units: ") + std::to_string(built) + " built, " +
              std::to_string(current) + " current";
    if (failed) summary += ", " + std::to_string(failed) + " failed";
    if (status != 0) {
        err = "the ahead-of-time compile " + std::string(failed ? "left units to the JIT" : "failed") + " (exit " +
              std::to_string(status) + "; see " + log + ")";
        return false;
    }
    return true;
}

std::string describeAppExe(const std::string& exePath, const std::string& iconPng) {
    std::string out;
    AppExeIdentity id;
    if (!readAppExeIdentity(exePath, id)) return exePath + ": no app identity (not an app executable bro built)\n";
    out += "executable " + exePath + "\n";
    out += serializeAppExeIdentity(id);
    const AppExeState s = appExeState(exePath, id.app);
    out += std::string("state=") + (s == AppExeState::Current ? "current" : appExeStateName(s)) + "\n";
    if (s == AppExeState::StaleEngine) out += "this-engine=" + engineVersion() + "\n";
    const std::string aotDir = appExeAotDir(exePath);
    const auto aot = readAotSummary(aotDir);
    if (!aot.empty()) {
        out += "aot.dir=" + aotDir + "\n";
        for (const auto& [k, v] : aot) out += "aot." + k + "=" + v + "\n";
    }
    out += describeWindowsExe(exePath, id, iconPng);
    return out;
}

void removeAppExe(const std::string& exePath) {
    const fs::path exe = u8path(exePath);
    std::error_code ec;
    fs::remove(exe, ec);
    if (ec) {
        // Windows: running. Rename it aside; the next build clears it.
        fs::path aside = exe;
        aside += ".old-removed";
        fs::rename(exe, aside, ec);
    }
    removeLeftovers(exe);
    // A module a running copy has mapped refuses removal on Windows; the
    // rest goes, and the next build of this executable rewrites the record.
    fs::remove_all(u8path(appExeAotDir(exePath)), ec);
}

#ifndef _WIN32
// Windows-only steps (app_exe_win.cpp): nothing to do elsewhere. A desktop
// entry is ELF's equivalent of both, written by app_install.cpp.
bool stampWindowsResources(const std::string&, const AppDescriptor&, const std::string&, const std::string&,
                           std::string&) {
    return true;
}
bool writeStartMenuShortcut(const std::string&, const std::string&, const std::string&, const std::string&, bool,
                            std::string&) {
    return true;
}
void removeStartMenuShortcut(const std::string&, bool) {}
std::string startMenuShortcutPath(const std::string&, bool) { return {}; }
std::string describeWindowsExe(const std::string&, const AppExeIdentity&, const std::string&) { return {}; }
#endif

}  // namespace bro::engine
