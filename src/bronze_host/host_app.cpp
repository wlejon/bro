// bro.app: the running app's identity, launch context and per-app directories
// (docs/app-api.js, docs/apps.md), over engine/app_runtime.h.
//
//   bro.app.id / name / version / description / icon / dir / exePath
//   bro.app.argv / cwd                      what this launch was given
//   bro.app.configDir / dataDir / cacheDir  created on first read
//   bro.app.logFile                         where stdout/stderr go ("" for none)
//   bro.app.singleInstance                  this process owns the instance channel
//   bro.app.manifest                        the declared desktop keys
//   bro.app.permissions                     { requested, granted, shell }
//   bro.app.spawn(args, {newInstance})      start another process of this app
//   bro.app.open(id, args, {newInstance})   start another app, found by id
//   bro.app.find(id)                        where open(id) would find it
//   instance events                         js/bro_core.js, over _setInstanceDispatcher
//
// Everything here is the same for an app.dll and a page compiled at boot.

#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "engine/app_manifest.h"
#include "engine/app_runtime.h"
#include "engine/config_loader.h"
#include "engine/engine.h"
#include "platform/window.h"  // platform::Window::raise()
#include "util/log.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
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
#else
#include <cerrno>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

extern "C" bool bro_window_simulateSingleInstance(const char* name, const char* argsJson);

namespace bro::bronze_host {

namespace {

namespace fs = std::filesystem;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string native(const std::string& p) {
    if (p.empty()) return p;
    std::u8string u = u8path(p).make_preferred().u8string();
    return std::string(u.begin(), u.end());
}

std::string ensureDir(const std::string& dir) {
    if (dir.empty()) return dir;
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);
    if (ec) LOG_WARN("bro.app: cannot create %s: %s", dir.c_str(), ec.message().c_str());
    return native(dir);
}

std::string exePath() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    int len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), int(buf.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), int(buf.size()), out.data(), len, nullptr, nullptr);
    return out;
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    std::error_code ec;
    fs::path p = fs::canonical(buf, ec);
    return ec ? std::string(buf) : p.string();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
#endif
}

Value strings(const std::vector<std::string>& items) {
    return hostArrayOf(items.size(), [&items](size_t i) -> Value { return ev::fromUtf8(items[i]); });
}

std::vector<std::string> readStrings(Value v) {
    std::vector<std::string> out;
    if (!ev::isObject(v)) return out;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return out;
    double n = ev::toDouble(lenV);
    if (!(n > 0) || n > 65536) return out;
    for (uint32_t i = 0; i < uint32_t(n); ++i) {
        Value e = ev::getElement(root.get(), i);
        out.push_back(ev::isUndefined(e) || ev::isNull(e) ? std::string() : ev::toUtf8(e));
    }
    return out;
}

std::string jsonString(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

// ---- spawn -------------------------------------------------------------------

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// One argument as CommandLineToArgvW reads it back.
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') out.append(backslashes * 2 + 1, L'\\');
        else out.append(backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

bool spawnDetached(const std::vector<std::string>& argv, const std::string& cwd, std::string& err) {
    std::wstring cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoteArg(widen(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring wcwd = widen(cwd);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        err = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
#else
bool spawnDetached(const std::vector<std::string>& argv, const std::string& cwd, std::string& err) {
    // Everything the child touches is built before fork: between fork and
    // exec only async-signal-safe calls are allowed in a threaded process.
    std::vector<char*> av;
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    const char* dir = cwd.empty() ? nullptr : cwd.c_str();
    pid_t pid = fork();
    if (pid < 0) { err = "fork failed"; return false; }
    if (pid == 0) {
        // The grandchild is reparented to init: no zombie for us to reap, and
        // its own session so closing this app does not take it down.
        setsid();
        pid_t again = fork();
        if (again == 0) {
            if (dir && chdir(dir) != 0) { /* keep the inherited cwd */ }
            execv(av[0], av.data());
            _exit(127);
        }
        _exit(again < 0 ? 1 : 0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { err = "fork failed"; return false; }
    return true;
}
#endif

// ---- open: another app by id ---------------------------------------------------

std::string u8str(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The folder app `id` beside this one: a directory of the project this app
// belongs to (BRO_PROJECT_ROOT, else the nearest project above the app, else
// the folder holding the app) whose bro.json declares that id, or which is
// named for it.
std::string findSiblingApp(const std::string& id, const engine::AppRuntimeInfo& self) {
    std::string root;
    if (const char* env = std::getenv("BRO_PROJECT_ROOT"); env && *env) root = env;
    if (root.empty() && !self.dir.empty()) root = engine::findAncestorProjectRoot(self.dir);
    if (root.empty() && !self.dir.empty()) root = u8str(u8path(self.dir).parent_path());
    if (root.empty()) return {};
    std::error_code ec;
    fs::directory_iterator it(u8path(root), fs::directory_options::skip_permission_denied, ec);
    if (ec) return {};
    std::string byName;
    for (const fs::directory_entry& e : it) {
        if (!e.is_directory(ec)) continue;
        const fs::path dir = e.path();
        const fs::path manifest = dir / "bro.json";
        if (fs::is_regular_file(manifest, ec)) {
            engine::AppDescriptor d;
            if (engine::parseAppManifest(u8str(manifest), d) && engine::appIdFor(d, u8str(dir)) == id)
                return u8str(dir);
        } else if (byName.empty() && u8str(dir.filename()) == id && fs::is_regular_file(dir / "index.html", ec)) {
            byName = u8str(dir);
        }
    }
    return byName;
}

struct AppTarget {
    std::string dir;
    std::string from;  // "installed" | "project" | "self"
};

// Where `bro <id>` finds the app (the install roots, docs/apps.md), else a
// sibling in this app's project.
AppTarget resolveAppId(const std::string& id) {
    const engine::AppRuntimeInfo& self = engine::currentApp();
    if (!engine::isValidAppId(id)) return {};
    if (std::string dir = engine::findInstalledApp(id); !dir.empty()) return {native(dir), "installed"};
    if (std::string dir = findSiblingApp(id, self); !dir.empty()) return {native(dir), "project"};
    if (id == self.id && !self.dir.empty()) return {native(self.dir), "self"};
    return {};
}

// The stock bro beside this executable: bro-headless and a host that embeds
// the engine open apps with the `bro` they ship with, which is what runs a
// folder app on its own.
std::string stockBro() {
    const std::string self = exePath();
    if (self.empty()) return self;
    fs::path p = u8path(self);
    std::string stem = u8str(p.stem());
    if (stem == "bro") return self;
#ifdef _WIN32
    fs::path bro = p.parent_path() / "bro.exe";
#else
    fs::path bro = p.parent_path() / "bro";
#endif
    std::error_code ec;
    return fs::is_regular_file(bro, ec) ? native(u8str(bro)) : self;
}

struct AppOpen {
    std::string id;
    std::string dir;
    std::string from;
    std::vector<std::string> command;
    std::string cwd;
    bool spawned = false;
};
std::vector<AppOpen>& appOpens() {
    static std::vector<AppOpen> opens;
    return opens;
}

Value appOpenValue(const AppOpen& o) {
    ObjectBuilder b;
    b.set("id", ev::fromUtf8(o.id));
    b.set("dir", ev::fromUtf8(o.dir));
    b.set("from", ev::fromUtf8(o.from));
    {
        ev::Persistent cmd(strings(o.command));
        b.set("command", cmd.get());
    }
    {
        // The app's own arguments: what it reads as bro.app.argv.
        std::vector<std::string> args;
        size_t first = 2;
        if (o.command.size() > 1 && o.command[1] == "--new-instance") first = 3;
        if (o.command.size() > first) args.assign(o.command.begin() + first, o.command.end());
        ev::Persistent a(strings(args));
        b.set("args", a.get());
    }
    b.set("cwd", ev::fromUtf8(o.cwd));
    b.set("spawned", ev::fromBool(o.spawned));
    return b.get();
}

Value makeErrorValue(const std::string& message) {
    ev::Persistent text(ev::fromUtf8(message));
    auto ctor = ev::globalValue("Error");
    if (ctor.found && ev::isFunction(ctor.value)) {
        ev::Persistent c(ctor.value);
        const Value arg = text.get();
        auto r = ev::construct(c.get(), std::span<const Value>(&arg, 1));
        if (!r.thrown) return r.value;
    }
    return text.get();
}

// The JS side's dispatcher (bro_core.js): called with (argvArray, cwd).
ev::Persistent* g_instanceDispatcher = nullptr;

void onInstance(const std::vector<std::string>& argv, const std::string& cwd) {
    if (g_instanceDispatcher) {
        ev::Persistent fn(g_instanceDispatcher->get());
        ev::Persistent a(strings(argv));
        ev::Persistent c(ev::fromUtf8(cwd));
        Value args[2] = {a.get(), c.get()};
        ev::call(fn.get(), ev::undefined(), args);
    }
    // The older bro.window.requestSingleInstance({onInstance}) hears it too.
    std::string json = "[";
    for (size_t i = 0; i < argv.size(); ++i) json += (i ? "," : "") + jsonString(argv[i]);
    json += "]";
    bro_window_simulateSingleInstance("", json.c_str());
    // A launch that handed off asked to see the app: bring it forward.
    if (auto* eng = hostEngine()) {
        if (auto* w = eng->window()) w->raise();
    }
}

}  // namespace

Value makeBroAppValue() {
    const engine::AppRuntimeInfo& app = engine::currentApp();
    const engine::AppDescriptor& m = app.manifest;
    ObjectBuilder o;
    o.set("id", ev::fromUtf8(app.id));
    o.set("name", ev::fromUtf8(m.name.empty() ? app.id : m.name));
    o.set("version", ev::fromUtf8(m.version));
    o.set("description", ev::fromUtf8(m.description));
    {
        std::string icon;
        if (!m.icon.empty() && !app.dir.empty()) {
            std::error_code ec;
            fs::path p = u8path(app.dir) / u8path(m.icon);
            if (fs::exists(p, ec)) {
                std::u8string u = p.lexically_normal().make_preferred().u8string();
                icon.assign(u.begin(), u.end());
            }
        }
        o.set("icon", ev::fromUtf8(icon));
    }
    o.set("dir", ev::fromUtf8(native(app.dir)));
    o.set("exePath", ev::fromUtf8(exePath()));
    {
        ev::Persistent argv(strings(app.argv));
        o.set("argv", argv.get());
    }
    o.set("cwd", ev::fromUtf8(native(app.cwd)));
    o.accessor("configDir", [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8(ensureDir(engine::currentApp().dirs.config));
    }, nullptr);
    o.accessor("dataDir", [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8(ensureDir(engine::currentApp().dirs.data));
    }, nullptr);
    o.accessor("cacheDir", [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8(ensureDir(engine::currentApp().dirs.cache));
    }, nullptr);
    o.accessor("logFile", [](Value, std::span<const Value>) -> Value {
        return ev::fromUtf8(native(engine::currentApp().logFile));
    }, nullptr);
    o.accessor("singleInstance", [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(engine::currentApp().singleInstance);
    }, nullptr);
    // How long this launch took, in ms from just before main(): the page
    // loaded (its scripts ran) and its first frame shown (-1 until then;
    // headless counts its first layout after load). The log's "first frame".
    o.accessor("startup", [](Value, std::span<const Value>) -> Value {
        ObjectBuilder s;
        s.set("loadedMs", ev::fromDouble(engine::documentLoadedMs()));
        s.set("firstFrameMs", ev::fromDouble(engine::firstFrameMs()));
        // How the graphics came up: the window, the GPU, and both (headless
        // does the two at once).
        const engine::GraphicsStartup g = engine::graphicsStartup();
        ObjectBuilder gfx;
        gfx.set("windowMs", ev::fromDouble(g.windowMs));
        gfx.set("gpuMs", ev::fromDouble(g.gpuMs));
        gfx.set("totalMs", ev::fromDouble(g.totalMs));
        s.set("graphics", gfx.get());
        return s.get();
    }, nullptr);

    {
        ObjectBuilder man;
        man.set("singleInstance", ev::fromBool(m.singleInstance));
        man.set("display", ev::fromBool(m.display));
        {
            ev::Persistent v(strings(m.categories));
            man.set("categories", v.get());
        }
        {
            ev::Persistent v(strings(m.keywords));
            man.set("keywords", v.get());
        }
        {
            ev::Persistent v(hostArrayOf(m.fileTypes.size(), [&m](size_t i) -> Value {
                ObjectBuilder t;
                t.set("name", ev::fromUtf8(m.fileTypes[i].name));
                { ev::Persistent x(strings(m.fileTypes[i].mimeTypes)); t.set("mimeTypes", x.get()); }
                { ev::Persistent x(strings(m.fileTypes[i].extensions)); t.set("extensions", x.get()); }
                return t.get();
            }));
            man.set("fileTypes", v.get());
        }
        {
            ev::Persistent v(hostArrayOf(m.actions.size(), [&m](size_t i) -> Value {
                ObjectBuilder a;
                a.set("id", ev::fromUtf8(m.actions[i].id));
                a.set("name", ev::fromUtf8(m.actions[i].name));
                { ev::Persistent x(strings(m.actions[i].args)); a.set("args", x.get()); }
                a.set("icon", ev::fromUtf8(m.actions[i].icon));
                return a.get();
            }));
            man.set("actions", v.get());
        }
        o.set("manifest", man.get());
    }

    {
        ObjectBuilder perms;
        auto* eng = hostEngine();
        std::vector<std::string> granted;
        for (const auto& ns : m.permissions)
            if (eng && eng->hasPrivilege(ns)) granted.push_back(ns);
        { ev::Persistent v(strings(m.permissions)); perms.set("requested", v.get()); }
        { ev::Persistent v(strings(granted)); perms.set("granted", v.get()); }
        perms.set("shell", ev::fromBool(eng && eng->isShellApp()));
        o.set("permissions", perms.get());
    }

    // spawn(args = [], {newInstance}) -> bool: another process of this app,
    // detached, from this app's working directory. With newInstance, it skips
    // the single-instance hand-off and opens a window of its own.
    o.def("spawn", 2, [](Value, std::span<const Value> a) -> Value {
        const engine::AppRuntimeInfo& app = engine::currentApp();
        auto* eng = hostEngine();
        if (eng && eng->displayMode() == engine::DisplayMode::Headless) return ev::fromBool(false);
        std::vector<std::string> argv{exePath()};
        bool newInstance = false;
        if (hasArg(a, 1) && ev::isObject(a[1])) newInstance = ev::toBool(ev::getProperty(a[1], "newInstance"));
        if (newInstance) argv.push_back("--new-instance");
        argv.push_back(app.dir);
        std::vector<std::string> extra = hasArg(a, 0) ? readStrings(a[0]) : std::vector<std::string>{};
        // The app's own arguments follow the directory (docs/apps.md: bro
        // takes only its own few flags out of them).
        argv.insert(argv.end(), extra.begin(), extra.end());
        std::string err;
        if (!spawnDetached(argv, app.cwd, err)) {
            LOG_WARN("bro.app.spawn: %s", err.c_str());
            return ev::fromBool(false);
        }
        return ev::fromBool(true);
    });

    // find(id) -> { id, dir, from } | null: where open(id) would find the app.
    o.def("find", 1, [](Value, std::span<const Value> a) -> Value {
        if (!hasArg(a, 0) || !ev::isString(a[0])) return ev::null();
        const std::string id = ev::toUtf8(a[0]);
        AppTarget t = resolveAppId(id);
        if (t.dir.empty()) return ev::null();
        ObjectBuilder b;
        b.set("id", ev::fromUtf8(id));
        b.set("dir", ev::fromUtf8(t.dir));
        b.set("from", ev::fromUtf8(t.from));
        return b.get();
    });

    // open(id, args = [], {newInstance}) -> Promise<{ id, dir, from, command,
    // args, cwd, spawned }>: another folder app, found as `bro <id>` finds it
    // (installed), else beside this one in its project, started with the
    // stock bro: `bro <dir> args...`. A single-instance app that is running
    // gets an `instance` event instead of a second window (bro does the
    // hand-off). Headless records the call (openedApps()) and starts nothing.
    o.def("open", 3, [](Value, std::span<const Value> a) -> Value {
        ev::Persistent promise(ev::createPromise());
        if (!hasArg(a, 0) || !ev::isString(a[0])) {
            ev::Persistent err(makeErrorValue("bro.app.open: an app id is required"));
            ev::rejectPromise(promise.get(), err.get());
            return promise.get();
        }
        const std::string id = ev::toUtf8(a[0]);
        std::vector<std::string> extra = hasArg(a, 1) ? readStrings(a[1]) : std::vector<std::string>{};
        bool newInstance = false;
        if (hasArg(a, 2) && ev::isObject(a[2])) newInstance = ev::toBool(ev::getProperty(a[2], "newInstance"));

        AppTarget t = resolveAppId(id);
        if (t.dir.empty()) {
            ev::Persistent err(makeErrorValue(
                engine::isValidAppId(id) ? "bro.app.open: no app '" + id + "' is installed or in this app's project"
                                         : "bro.app.open: '" + id + "' is not an app id"));
            ev::rejectPromise(promise.get(), err.get());
            return promise.get();
        }
        AppOpen rec;
        rec.id = id;
        rec.dir = t.dir;
        rec.from = t.from;
        rec.command.push_back(stockBro());
        if (newInstance) rec.command.push_back("--new-instance");
        rec.command.push_back(t.dir);
        rec.command.insert(rec.command.end(), extra.begin(), extra.end());
        rec.cwd = engine::currentWorkingDirectory();

        auto* eng = hostEngine();
        const bool headless = eng && eng->displayMode() == engine::DisplayMode::Headless;
        if (!headless) {
            std::string err;
            if (!spawnDetached(rec.command, rec.cwd, err)) {
                LOG_WARN("bro.app.open %s: %s", id.c_str(), err.c_str());
                ev::Persistent e(makeErrorValue("bro.app.open: " + err));
                ev::rejectPromise(promise.get(), e.get());
                return promise.get();
            }
            rec.spawned = true;
        }
        appOpens().push_back(rec);
        ev::Persistent result(appOpenValue(rec));
        ev::resolvePromise(promise.get(), result.get());
        return promise.get();
    });

    o.def("_setInstanceDispatcher", 1, [](Value, std::span<const Value> a) -> Value {
        // The app's top-level page hears launches; an <iframe>'s realm runs
        // bro_core.js too and must not take them over.
        if (isChildRealm()) return ev::undefined();
        if (!g_instanceDispatcher) g_instanceDispatcher = new ev::Persistent();
        g_instanceDispatcher->set(hasArg(a, 0) ? a[0] : ev::undefined());
        engine::setAppInstanceHandler(onInstance);
        return ev::undefined();
    });
    // A test's stand-in for a second launch: delivered as a real one is.
    o.def("_simulateInstance", 2, [](Value, std::span<const Value> a) -> Value {
        std::vector<std::string> argv = hasArg(a, 0) ? readStrings(a[0]) : std::vector<std::string>{};
        std::string cwd = hasArg(a, 1) ? ev::toUtf8(a[1]) : engine::currentWorkingDirectory();
        engine::simulateAppInstance(argv, cwd);
        return ev::undefined();
    });
    return o.get();
}

Value appOpenRecordsValue() {
    const auto& opens = appOpens();
    return hostArrayOf(opens.size(), [&opens](size_t i) -> Value { return appOpenValue(opens[i]); });
}

void clearAppOpenRecords() { appOpens().clear(); }

}  // namespace bro::bronze_host
