#include "engine/engine.h"
#include "engine/config_loader.h"
#include "util/interrupt.h"
#include "util/log.h"

#include "broaudio/log.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
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
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
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
static void redirectLogToFile() {
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
    int fd = _sopen("bro.log", _O_WRONLY | _O_CREAT | _O_TRUNC, _SH_DENYWR, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        char fallback[64];
        std::snprintf(fallback, sizeof(fallback), "bro-%lu.log", static_cast<unsigned long>(_getpid()));
        fd = _sopen(fallback, _O_WRONLY | _O_CREAT | _O_TRUNC, _SH_DENYWR, _S_IREAD | _S_IWRITE);
        if (fd < 0) return;
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
    FILE* f = freopen("bro.log", "w", stderr);
    if (!f) return;
    dup2(fileno(stderr), fileno(stdout));
#endif
    setvbuf(stderr, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);
}

// Get the directory containing the current executable.
static std::string exeDir() {
    std::string path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) path = std::string(buf, len);
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(buf, resolved)) path = resolved;
        else path = buf;
    }
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; path = buf; }
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}

// Check if a file exists.
static bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

using bro::engine::parseConfig;
using bro::engine::findAncestorProjectRoot;

static void printUsage() {
    fprintf(stderr,
        "bro -- lightweight HTML/CSS/JS app runtime\n"
        "\n"
        "Usage: bro <app-directory>\n"
        "\n"
        "Loads index.html from the given directory and runs it in a\n"
        "GPU-accelerated window (Skia + OpenGL via SDL3).\n"
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
        "\n"
        "Additional bro.json options:\n"
        "  vsync (bool), resizable (bool), maxFps (number),\n"
        "  splash (bool, default true),\n"
        "  scrollSpeed (number), doubleClickThreshold (ms),\n"
        "  doubleClickDistance (px)\n"
        "\n"
        "See also: bro-headless for scripted/headless mode.\n");
}

int main(int argc, char* argv[]) {
    redirectLogToFile();

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

    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printUsage();
        return 0;
    }

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

    bro::engine::EngineConfig config;

    // CLI flags parsed up-front (so bro.json values can still override on
    // purpose, and --no-splash wins as a final override applied after).
    bool cliNoSplash = false;
    bool cliSplash   = false;
    std::vector<const char*> posArgs;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-splash") == 0)     cliNoSplash = true;
        else if (strcmp(argv[i], "--splash") == 0)   cliSplash   = true;
        else posArgs.push_back(argv[i]);
    }

    // Settings persist next to the executable
    std::string settingsDir = exeDir();
    config.settingsPath = settingsDir + "/.bro_settings.json";

    // Expose exe directory to JS (process.env.BRO_EXE_DIR) so apps like the
    // launcher can locate sibling executables (bro, bro-headless, bro-server).
    // Windows uses _putenv_s so the CRT's getenv() reflects the change.
#ifdef _WIN32
    _putenv_s("BRO_EXE_DIR", settingsDir.c_str());
#else
    setenv("BRO_EXE_DIR", settingsDir.c_str(), 1);
#endif

    // Resolve launch target → projectRoot + appDir. Three modes:
    //   bro                          (no args) — exe dir is the target
    //   bro <path>                   path is either an app dir, a project dir,
    //                                or a project bro.json file
    //   bro <project.json> --app X   (future, not parsed here yet)
    //
    // A bro.json with `default_app`, `lib`, or `system` keys is treated as a
    // project manifest: its directory becomes the projectRoot, and `app`/
    // `default_app` resolves against it.
    auto isJsonFile = [](const std::string& s) {
        return s.size() >= 5 && s.substr(s.size() - 5) == ".json";
    };
    auto dirOf = [](const std::string& p) -> std::string {
        size_t i = p.find_last_of("/\\");
        return (i == std::string::npos) ? std::string(".") : p.substr(0, i);
    };
    auto isAbsolute = [](const std::string& p) {
        return !p.empty() && (p[0] == '/' || p[0] == '\\' ||
                              (p.size() >= 2 && p[1] == ':'));
    };

    if (!posArgs.empty()) {
        std::string target = posArgs[0];

        if (isJsonFile(target) && fileExists(target)) {
            // Explicit project (or app) bro.json launch.
            bool isProject = false;
            parseConfig(target, config, &isProject);
            std::string targetDir = dirOf(target);
            if (isProject) {
                config.projectRoot = targetDir;
                if (config.appDir.empty()) config.appDir = targetDir;
                else if (!isAbsolute(config.appDir)) config.appDir = targetDir + "/" + config.appDir;
            } else {
                // App manifest pointed to directly — its dir is the appDir.
                config.appDir = targetDir;
            }
        } else {
            // Directory target. Don't preset config.appDir before parseConfig
            // — the empty-check guard in config_loader skips default_app when
            // appDir is already set.
            std::string broJson = target + "/bro.json";
            if (fileExists(broJson)) {
                bool isProject = false;
                parseConfig(broJson, config, &isProject);
                if (isProject) {
                    config.projectRoot = target;
                    if (config.appDir.empty()) config.appDir = target;
                    else if (!isAbsolute(config.appDir)) config.appDir = target + "/" + config.appDir;
                } else {
                    // Plain app bro.json — its dir is the appDir.
                    config.appDir = target;
                }
            } else {
                // No bro.json — treat directory as the app dir directly.
                config.appDir = target;
            }
        }
    } else {
        // No arguments — auto-detect from exe directory.
        std::string dir = exeDir();
#ifndef _WIN32
        if (!dir.empty() && dir != ".") chdir(dir.c_str());
#endif
        std::string configPath = dir + "/bro.json";

        if (fileExists(configPath)) {
            bool isProject = false;
            parseConfig(configPath, config, &isProject);
            if (isProject) {
                config.projectRoot = dir;
            }
            if (config.appDir.empty()) {
                config.appDir = dir;
            } else if (!isAbsolute(config.appDir)) {
                config.appDir = dir + "/" + config.appDir;
            }
        } else if (fileExists(dir + "/index.html")) {
            config.appDir = dir;
        } else if (fileExists(dir + "/system/projects/index.html")) {
            // No app next to the exe — fall back to the built-in project
            // manager. Ships under system/projects/ in every release.
            config.appDir = dir + "/system/projects";
        } else {
            printUsage();
            return 1;
        }
    }

    // Inherit projectRoot from a parent bro process when this app was spawned
    // from a launcher running inside a project.
    if (config.projectRoot.empty()) {
        if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
            if (*env) config.projectRoot = env;
        }
    }

    // Still nothing: the app was launched by passing its own directory
    // directly (this file's own documented usage) and its bro.json carries
    // no project keys itself. Walk up looking for an ancestor project
    // manifest so /lib, /system, /std, /app mounts still resolve standalone.
    if (config.projectRoot.empty() && !config.appDir.empty()) {
        config.projectRoot = findAncestorProjectRoot(config.appDir);
    }

    // When launching via a project bro.json, the app's own bro.json also
    // provides per-app overrides (title, width, height, etc.). Parse it
    // after the project manifest so app-level keys take precedence. Preserve
    // the resolved appDir against the app manifest's `"app": "."` field.
    if (!config.projectRoot.empty() && !config.appDir.empty()) {
        std::string appBroJson = config.appDir + "/bro.json";
        if (fileExists(appBroJson) && appBroJson != config.projectRoot + "/bro.json") {
            std::string preservedAppDir = config.appDir;
            parseConfig(appBroJson, config, nullptr);
            config.appDir = preservedAppDir;
        }
    }

    // CLI splash overrides — applied last so they win over bro.json.
    if (cliNoSplash) config.showSplash = false;
    if (cliSplash)   config.showSplash = true;

    // Expose the resolved app directory and project root to JS / child
    // processes (process.env.BRO_APP_DIR, BRO_PROJECT_ROOT) so the launcher
    // and other meta-apps can locate themselves and their siblings on disk
    // without guessing from cwd, and so spawned children inherit the project
    // context. Resolved to absolute paths.
    auto absolutize = [&](const std::string& p) -> std::string {
        if (p.empty()) return p;
#ifdef _WIN32
        char abs[MAX_PATH];
        return _fullpath(abs, p.c_str(), MAX_PATH) ? std::string(abs) : p;
#else
        char abs[PATH_MAX];
        return realpath(p.c_str(), abs) ? std::string(abs) : p;
#endif
    };

    config.appDir = absolutize(config.appDir);
    if (!config.projectRoot.empty()) config.projectRoot = absolutize(config.projectRoot);

#ifdef _WIN32
    _putenv_s("BRO_APP_DIR", config.appDir.c_str());
    _putenv_s("BRO_PROJECT_ROOT", config.projectRoot.c_str());
#else
    setenv("BRO_APP_DIR", config.appDir.c_str(), 1);
    if (!config.projectRoot.empty()) setenv("BRO_PROJECT_ROOT", config.projectRoot.c_str(), 1);
    else unsetenv("BRO_PROJECT_ROOT");
#endif

    try {
        bro::engine::Engine engine(config);
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
