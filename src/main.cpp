#include "engine/engine.h"
#include "engine/config_loader.h"
#include "engine/launcher.h"
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
        "  doubleClickDistance (px),\n"
        "  borderless (bool), alwaysOnTop (bool),\n"
        "  minWidth/minHeight/maxWidth/maxHeight (px resize limits),\n"
        "  windowX/windowY (px startup position), display (index to center on)\n"
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

    // Settings persist next to the executable.
    config.settingsPath = bro::engine::executableDir() + "/.bro_settings.json";

    // Resolve launch target → projectRoot + appDir. The target may be an app
    // directory, a project directory, or a bro.json of either kind; with no
    // argument the executable's own directory is probed. Shared with
    // bro-headless, bro-server and any host application that links
    // bro_engine — see engine/launcher.h.
    if (!bro::engine::resolveLaunchTarget(posArgs.empty() ? std::string()
                                                          : std::string(posArgs[0]),
                                          config)) {
        printUsage();
        return 1;
    }

    // CLI splash overrides — applied last so they win over bro.json.
    if (cliNoSplash) config.showSplash = false;
    if (cliSplash)   config.showSplash = true;

    // Absolutise and publish BRO_EXE_DIR / BRO_APP_DIR / BRO_PROJECT_ROOT so
    // JS and spawned children can locate themselves without guessing from cwd.
    bro::engine::publishLaunchEnv(config);

    try {
        bro::engine::Engine engine(config);
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
