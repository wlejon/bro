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
//   instance events                         js/bro_core.js, over _setInstanceDispatcher
//
// Everything here is the same for an app.dll and a page compiled at boot.

#include "bronze_host/host_bro_namespaces.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_runtime.h"
#include "bronze_host/host_values.h"
#include "engine/app_runtime.h"
#include "engine/engine.h"
#include "platform/window.h"  // platform::Window::raise()
#include "util/log.h"

#include <cstdio>
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

}  // namespace bro::bronze_host
