#include "engine/engine.h"
#include "engine/config_loader.h"
#include "engine/launcher.h"
#include "engine/app_runtime.h"
#include "util/exe_dir.h"
#include "util/interrupt.h"
#include "util/log.h"

#include "bronze_host/app_module.h"
#include "bronze_host/host_headless.h"
#include "render/vulkan_debug.h"
#include "platform/desktop_notifications.h"
#include <optional>

#include "broaudio/log.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#include <process.h>
#include <crtdbg.h>
#else
#include <unistd.h>
#include <climits>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#endif

// bro.exe is /SUBSYSTEM:WINDOWS, so stdout/stderr go nowhere by default. Send
// them to bro.log in the current working directory — the place the user ran
// bro from — so LOG_* and console.* output is captured side-by-side with the
// invocation.
//
// The launcher app spawns child bro processes with the same cwd, so multiple
// bro.exe instances race for bro.log. We open with exclusive write sharing:
// the first instance wins bro.log, and any concurrent instance falls back to
// bro-<pid>.log so its logs aren't lost and stderr stays valid (a failed
// freopen would close stderr and any subsequent stdio call would crash).
//
// One file descriptor is dup'd to both stderr and stdout so the two streams
// share a kernel write position and don't fight over file size.
// Retention for the fallback logs: every launch that finds the main log taken
// (a --new-instance launch, a second launcher child) writes <stem>-<pid>.log,
// and nothing else ever removes those. Before taking a log, delete the
// <stem>-<pid>.log files beside it that no process is writing, keeping the
// newest kKeepInstanceLogs of them for a look after a crash. A log still being
// written is never touched: on Windows its writer opened it without
// FILE_SHARE_DELETE, so the delete fails; on POSIX its writer holds the flock.
static constexpr size_t kKeepInstanceLogs = 5;

static void pruneInstanceLogs(const std::string& dir, const std::string& stem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path d = dir.empty() ? fs::path(".") : fs::path(std::u8string(dir.begin(), dir.end()));
    const std::string prefix = stem + "-";
    struct Stale { fs::path path; fs::file_time_type time; };
    std::vector<Stale> stale;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        const std::u8string u = it->path().filename().u8string();
        const std::string name(u.begin(), u.end());
        if (name.size() <= prefix.size() + 4 || name.compare(0, prefix.size(), prefix) != 0 ||
            name.compare(name.size() - 4, 4, ".log") != 0)
            continue;
        const std::string pid = name.substr(prefix.size(), name.size() - prefix.size() - 4);
        if (pid.find_first_not_of("0123456789") != std::string::npos) continue;
        std::error_code tec;
        if (!it->is_regular_file(tec)) continue;
#ifndef _WIN32
        // In use: its writer holds the flock (see openLocked below).
        int fd = open(it->path().c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) continue;
        const bool idle = flock(fd, LOCK_EX | LOCK_NB) == 0;
        close(fd);
        if (!idle) continue;
#endif
        stale.push_back({it->path(), it->last_write_time(tec)});
    }
    if (stale.size() <= kKeepInstanceLogs) return;
    std::sort(stale.begin(), stale.end(), [](const Stale& a, const Stale& b) { return a.time > b.time; });
    for (size_t i = kKeepInstanceLogs; i < stale.size(); ++i) {
        std::error_code rec;
        fs::remove(stale[i].path, rec);  // fails, harmlessly, for one a writer has open (Windows)
    }
}

// `logPath` is the file to take ("bro.log" in the working directory, or an
// app's own log), `fallbackStem` + "-<pid>.log" beside it the one to use when
// another process already writes there. Returns the path actually opened, ""
// when neither could be.
static std::string redirectLogToFile(const std::string& logPath = "bro.log",
                                     const std::string& fallbackStem = "bro") {
    std::string opened;
    const std::string dir = [&] {
        size_t i = logPath.find_last_of("/\\");
        return i == std::string::npos ? std::string() : logPath.substr(0, i + 1);
    }();
    pruneInstanceLogs(dir, fallbackStem);
#ifdef _WIN32
    // bro.exe is /SUBSYSTEM:WINDOWS; when launched from a non-console parent
    // (PowerShell Start-Process, the launcher's CreateProcess, double-click,
    // etc.) stderr/stdout have no backing fd — _fileno returns -2 and a
    // subsequent _dup2 silently fails. Reopen them onto NUL first so they
    // have valid fds we can _dup2 over.
    FILE* dummy = nullptr;
    freopen_s(&dummy, "NUL", "w", stderr);
    freopen_s(&dummy, "NUL", "w", stdout);

    // _SH_DENYWR: refuse the open if another writer already has the file.
    // Falls back to bro-<pid>.log on contention so launcher children don't
    // clobber the launcher's log.
    // The path is UTF-8 (an app's log dir holds the user's name): open it
    // through the wide API.
    auto openExclusive = [](const std::string& path) -> int {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring w(size_t(wlen > 0 ? wlen : 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wlen);
        return _wsopen(w.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC, _SH_DENYWR, _S_IREAD | _S_IWRITE);
    };
    int fd = openExclusive(logPath);
    opened = logPath;
    if (fd < 0) {
        opened = dir + fallbackStem + "-" + std::to_string(static_cast<unsigned long>(_getpid())) + ".log";
        fd = openExclusive(opened);
        if (fd < 0) return {};
    }
    _dup2(fd, _fileno(stderr));
    _dup2(fd, _fileno(stdout));
    _close(fd);

    // Mirror at the Win32 API level so anything bypassing CRT stdio
    // (OutputDebugString-free SDL paths, third-party libs that call
    // GetStdHandle directly) lands in the same file.
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr)));
    if (h != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_ERROR_HANDLE, h);
        SetStdHandle(STD_OUTPUT_HANDLE, h);
    }
#else
    // Same contention policy as the _SH_DENYWR open above. POSIX has no
    // share modes, so the exclusive writer is an flock held for the life of
    // the process (it lives on the open file description, which the dup2s
    // below keep alive). Truncate only once the lock is ours: opening with
    // O_TRUNC first would wipe the log of the bro that already owns it.
    auto openLocked = [](const char* path) -> int {
        int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
        if (flock(fd, LOCK_EX | LOCK_NB) != 0 || ftruncate(fd, 0) != 0) {
            close(fd);
            return -1;
        }
        return fd;
    };
    int fd = openLocked(logPath.c_str());
    opened = logPath;
    if (fd < 0) {
        opened = dir + fallbackStem + "-" + std::to_string(static_cast<long>(getpid())) + ".log";
        fd = openLocked(opened.c_str());
        if (fd < 0) return {};
    }
    dup2(fd, fileno(stderr));
    dup2(fd, fileno(stdout));
    close(fd);
#endif
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
    return opened;
}

static void printUsage() {
    fprintf(stderr,
        "bro -- lightweight HTML/CSS/JS app runtime\n"
        "\n"
        "Usage: bro [flags] <app-directory | app-id> [app arguments...]\n"
        "       bro --install <app-directory> [--link] [--system] [--exec <bro>]\n"
        "       bro --uninstall <app-id> [--system]\n"
        "       bro --list-apps\n"
        "       bro --desktop-entry <app-directory>\n"
        "\n"
        "Loads index.html from the given directory and runs it in a\n"
        "GPU-accelerated window (Skia + Vulkan via SDL3). Everything after the\n"
        "directory is the app's (bro.app.argv), apart from the flags below\n"
        "before a `--`. An installed app can be named by its id. See docs/apps.md.\n"
        "\n"
        "Alternatively, place a bro.json config file or index.html\n"
        "next to the executable to run without arguments.\n"
        "\n"
        "Example:\n"
        "  bro ../broworkshop/demos/example\n"
        "\n"
        "bro.json format:\n"
        "  {\"app\": \".\", \"title\": \"My App\", \"width\": 1200, \"height\": 800}\n"
        "\n"
        "CLI flags:\n"
        "  --no-splash / --splash  Disable or force the startup splash screen.\n"
        "  --no-gpu                Run without Vulkan: CPU-rendered frames shown in a\n"
        "                          software window (no 3D scenes or WebGL).\n"
        "  --drm                   Run bare-metal on Linux DRM/KMS display with seat & libinput.\n"
        "  --new-instance          Start a new instance of a single-instance app instead of\n"
        "                          handing the arguments to the running one.\n"
        "  --notification <args>   Start as a click on one of the app's notifications\n"
        "                          (the page hears notificationclick; docs/sys-api.js).\n"
        "\n"
        "Additional bro.json options:\n"
        "  vsync (bool), resizable (bool), maxFps (number),\n"
        "  splash (bool, default true),\n"
        "  scrollSpeed (number), doubleClickThreshold (ms),\n"
        "  doubleClickDistance (px),\n"
        "  borderless (bool), alwaysOnTop (bool),\n"
        "  minWidth/minHeight/maxWidth/maxHeight (px resize limits),\n"
        "  windowX/windowY (px startup position), display (index to center on)\n"
        "\n"
        "See also: bro-headless for scripted/headless mode.\n");
}

// The command line after the program name, as UTF-8. On Windows main()'s argv
// is in the ANSI code page, which cannot carry a path or an argument in
// another script; the wide command line can.
static std::vector<std::string> utf8Args(int argc, char* argv[]) {
    std::vector<std::string> out;
#ifdef _WIN32
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (w) {
        for (int i = 1; i < n; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
            out.push_back(std::move(s));
        }
        LocalFree(w);
        return out;
    }
#endif
    for (int i = 1; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

// --help from a console: bro.exe is a GUI-subsystem program, so borrow the
// parent console for the text.
static void attachParentConsole() {
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

int main(int argc, char* argv[]) {
    const std::vector<std::string> args = utf8Args(argc, argv);

    // App management (`bro --install <dir>`, docs/apps.md) prints and exits,
    // and never starts an engine.
    if (int status = bro::engine::runAppCommand(args); status >= 0) return status;

    if (!args.empty() && (args[0] == "--help" || args[0] == "-h")) {
        attachParentConsole();
        printUsage();
        return 0;
    }

    // The command line: bro's own flags, the launch target, then the app's
    // arguments (bro.app.argv). bro's flags are taken from anywhere before a
    // `--`; everything else after the target is the app's, verbatim, `--`
    // included.
    bool cliNoSplash = false;
    bool cliSplash   = false;
    bool cliNoGpu    = false;
    bool cliDrm      = false;
    bool cliNewInstance = false;
    // A click on one of the app's notifications started this run: its
    // activation text (`--notification <args>`), or, from a Windows toast,
    // `--notification-activated` with the text to come through COM (which
    // also appends -Embedding).
    std::string cliNotification;
    bool cliNotificationCom = false;
    bool expectNotificationArgs = false;
    std::string target;
    bool haveTarget = false;
    std::vector<std::string> appArgs;
    bool dashDash = false;
    for (const std::string& a : args) {
        if (expectNotificationArgs) { cliNotification = a; expectNotificationArgs = false; continue; }
        if (!dashDash) {
            if (a == "--no-splash")    { cliNoSplash = true; continue; }
            if (a == "--splash")       { cliSplash = true; continue; }
            if (a == "--no-gpu")       { cliNoGpu = true; continue; }
            if (a == "--drm")          { cliDrm = true; continue; }
            if (a == "--new-instance") { cliNewInstance = true; continue; }
            if (a == "--notification") { expectNotificationArgs = true; continue; }
            if (a == "--notification-activated") { cliNotificationCom = true; continue; }
            if (cliNotificationCom && (a == "-Embedding" || a == "/Embedding")) continue;
        }
        if (!haveTarget) { target = a; haveTarget = true; continue; }
        if (a == "--") dashDash = true;
        appArgs.push_back(a);
    }

    bro::engine::EngineConfig config;
    // Settings persist next to the executable (or, in a macOS bundle, in the
    // user data dir); an app with an id of its own keeps them in its config
    // directory, below.
    config.settingsPath = bro::util::defaultSettingsPath();

    // Resolve launch target → projectRoot + appDir. The target may be an app
    // directory, a project directory, a bro.json of either kind, or an
    // installed app's id; with no argument the executable's own directory is
    // probed. Shared with bro-headless, bro-server and any host application
    // that links bro_engine — see engine/launcher.h.
    if (!bro::engine::resolveLaunchTarget(target, config)) {
        redirectLogToFile();
        printUsage();
        return 1;
    }
    config.appArgs = std::move(appArgs);
    config.launchCwd = bro::engine::currentWorkingDirectory();

    // CLI overrides — applied last so they win over bro.json.
    if (cliNoSplash) config.showSplash = false;
    if (cliSplash)   config.showSplash = true;
    if (cliNoGpu)    config.graphics.useGPU = false;
    if (cliDrm)      config.displayMode = bro::engine::DisplayMode::Drm;

    // Absolutise and publish BRO_EXE_DIR / BRO_APP_DIR / BRO_PROJECT_ROOT so
    // JS and spawned children can locate themselves without guessing from cwd,
    // and settle the app's id (bro.app, BRO_APP_ID).
    bro::engine::publishLaunchEnv(config);
    const bool appHasId = !config.manifest.id.empty();

    // Notification clicks (docs/sys-api.js 5a): set up before anything else
    // touches the desktop (macOS wants the notification center's delegate
    // before the app finishes launching; a toast's activation waits on the
    // activator this registers), and the click that started this run queued
    // for the page.
    {
        std::string exePath;
#ifdef _WIN32
        wchar_t buf[MAX_PATH * 4];
        const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        if (n > 0 && n < std::size(buf)) {
            const std::filesystem::path p(std::wstring(buf, n));
            const std::u8string u8 = p.u8string();
            exePath.assign(u8.begin(), u8.end());
        }
#endif
        bro::platform::desktop::initNotificationActivation(bro::engine::currentApp().id, exePath,
                                                           bro::engine::absolutePath(config.appDir),
                                                           cliNotificationCom);
        if (!cliNotification.empty()) bro::platform::desktop::noteLaunchNotification(cliNotification);
    }

    // A single-instance app (`"singleInstance": true`): a launch while one is
    // running hands its argv and working directory over and exits, before
    // any window, GPU or script work, so it costs a launcher next to nothing.
    // A DRM shell host is the session, never a second launch of something.
    if (config.manifest.singleInstance && !cliNewInstance &&
        config.displayMode != bro::engine::DisplayMode::Drm) {
        if (bro::engine::claimSingleInstance(config) == bro::engine::InstanceClaim::HandedOff) return 0;
    }

    // Logs: an app with an id writes its own log in its state directory
    // (bro.app.logFile); an anonymous app writes bro.log in the working
    // directory, as bro always has.
    {
        std::string logFile;
        if (appHasId) {
            const auto& dirs = bro::engine::currentApp().dirs;
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(std::u8string(
                dirs.logDir.begin(), dirs.logDir.end())), ec);
            logFile = redirectLogToFile(dirs.logFile, config.appId);
            if (logFile.empty()) logFile = redirectLogToFile();
            // The app's window and engine settings live with its config.
            std::filesystem::create_directories(std::filesystem::path(std::u8string(
                dirs.config.begin(), dirs.config.end())), ec);
            config.settingsPath = dirs.config + "/bro_settings.json";
        } else {
            logFile = redirectLogToFile();
        }
        if (!logFile.empty()) logFile = bro::engine::absolutePath(logFile);
        bro::engine::setCurrentAppLogFile(logFile);
    }

#if defined(_WIN32) && defined(_DEBUG)
    // Route the Debug CRT's assert()/error report dialogs ("Debug Error!
    // abort() has been called", Abort/Retry/Ignore) to stderr — which
    // redirectLogToFile just pointed at bro.log — instead of a modal box.
    // bro.exe is a GUI-subsystem app, so without this a Debug assertion
    // blocks invisible behind the game window instead of exiting. Unlike
    // bro-headless we deliberately do NOT call SetErrorMode(SEM_NOGPFAULT-
    // ERRORBOX): that would bypass WER and lose the %LOCALAPPDATA%\
    // CrashDumps minidumps used for post-mortem debugging.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

    bro::util::installSignalHandler();

    // Route broaudio's diagnostics through our logger so they land in bro.log
    // alongside everything else, instead of SDL_Log's default console sink.
    broaudio::setLogCallback([](broaudio::LogLevel level, const char* msg) {
        switch (level) {
            case broaudio::LogLevel::Info:  LOG_INFO("%s", msg);  break;
            case broaudio::LogLevel::Warn:  LOG_WARN("%s", msg);  break;
            case broaudio::LogLevel::Error: LOG_ERROR("%s", msg); break;
        }
    });

    // Does this app directory carry a compiled module? Asked BEFORE the Engine
    // is constructed because engine init uses the answer: it is what
    // distinguishes an app that declares `"compiled": true` and was opened by a
    // host that can run it from one opened by a host that cannot, and the two
    // want different diagnostics (engine_init.cpp). Loading it is the other
    // side of the Engine — see below.
    std::optional<std::string> appModule = bro::bronze_host::findAppModule(config.appDir);
    config.hostProvidesCompiledApp = appModule.has_value();

    int exitCode = 0;
    try {
        bro::engine::Engine engine(config);
        // The compiled top level runs here: after the Engine (the host globals
        // it reads are backed by it) and before the run loop (what it schedules
        // is what the run loop then drives).
        if (appModule) {
            bro::bronze_host::runAppModule(engine, *appModule);
        }
        engine.run();
        // An app that tests itself in a real window (tests/windowed) reports
        // the way a headless test does: a failed assert() exits 1, a
        // skipTest() 77.
        if (engine.hasTestFailure() || bro::bronze_host::hasTestFailure()) exitCode = 1;
        else if (bro::bronze_host::wasTestSkipped()) exitCode = 77;
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        exitCode = 1;
    }
    // Before static destruction: the channel's server thread is a static
    // std::thread, and destroying it still joinable would terminate().
    bro::engine::releaseSingleInstance();
    // A Vulkan validation error (BRO_VK_VALIDATION=1) fails the run, the
    // device teardown included.
    if (const uint32_t errors = bro::render::vulkanValidationErrorCount()) {
        fprintf(stderr, "Vulkan validation: %u error(s)\n", errors);
        exitCode = 1;
    }
    return exitCode;
}
