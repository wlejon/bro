#include "engine/engine.h"
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

// Minimal JSON parser for bro.json — extracts string and integer values.
static bool parseConfig(const std::string& path, bro::engine::EngineConfig& config) {
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    // Helper: extract a JSON string value for a given key.
    auto getString = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        // Skip whitespace and colon
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos >= content.size() || content[pos] != '"') return {};
        pos++; // skip opening quote
        std::string result;
        while (pos < content.size() && content[pos] != '"') {
            if (content[pos] == '\\' && pos + 1 < content.size()) {
                pos++;
                result += content[pos];
            } else {
                result += content[pos];
            }
            pos++;
        }
        return result;
    };

    // Helper: extract an integer value for a given key.
    auto getInt = [&](const char* key, int defaultVal) -> int {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return defaultVal;
        pos += needle.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos >= content.size()) return defaultVal;
        // Parse integer (possibly negative)
        std::string num;
        if (content[pos] == '-') { num += '-'; pos++; }
        while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') {
            num += content[pos++];
        }
        if (num.empty() || num == "-") return defaultVal;
        return std::stoi(num);
    };

    // Helper: extract a boolean value for a given key (-1 = not found).
    auto getBool = [&](const char* key) -> int {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return -1;
        pos += needle.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos + 4 <= content.size() && content.substr(pos, 4) == "true") return 1;
        if (pos + 5 <= content.size() && content.substr(pos, 5) == "false") return 0;
        return -1;
    };

    // Helper: extract a float value for a given key.
    auto getFloat = [&](const char* key, float defaultVal) -> float {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return defaultVal;
        pos += needle.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos >= content.size()) return defaultVal;
        std::string num;
        if (content[pos] == '-') { num += '-'; pos++; }
        while (pos < content.size() && (content[pos] >= '0' && content[pos] <= '9' || content[pos] == '.')) {
            num += content[pos++];
        }
        if (num.empty() || num == "-") return defaultVal;
        return std::stof(num);
    };

    std::string app = getString("app");
    if (!app.empty()) config.appDir = app;

    std::string title = getString("title");
    if (!title.empty()) config.title = title;

    // Graphics settings
    int w = getInt("width", 0);
    if (w > 0) config.graphics.width = w;

    int h = getInt("height", 0);
    if (h > 0) config.graphics.height = h;

    int vsync = getBool("vsync");
    if (vsync >= 0) config.graphics.vsync = (vsync == 1);

    int resizable = getBool("resizable");
    if (resizable >= 0) config.graphics.resizable = (resizable == 1);

    int splash = getBool("splash");
    if (splash >= 0) config.showSplash = (splash == 1);

    float maxFps = getFloat("maxFps", 0);
    if (maxFps > 0) config.graphics.maxFrameIntervalMs = 1000.0 / maxFps;

    // Input settings
    float scrollSpeed = getFloat("scrollSpeed", 0);
    if (scrollSpeed > 0) config.input.scrollSpeed = scrollSpeed;

    float dblClickTime = getFloat("doubleClickThreshold", 0);
    if (dblClickTime > 0) config.input.doubleClickThresholdMs = static_cast<double>(dblClickTime);

    float dblClickDist = getFloat("doubleClickDistance", 0);
    if (dblClickDist > 0) config.input.doubleClickDistancePx = dblClickDist;

    return true;
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
        "  doubleClickDistance (px)\n"
        "\n"
        "See also: bro-headless for scripted/headless mode.\n");
}

int main(int argc, char* argv[]) {
    redirectLogToFile();

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

    if (!posArgs.empty()) {
        // Explicit app directory argument
        config.appDir = posArgs[0];

        // Load bro.json from the app directory if present
        std::string appConfig = config.appDir + "/bro.json";
        if (fileExists(appConfig)) {
            parseConfig(appConfig, config);
            config.appDir = posArgs[0]; // preserve explicit appDir over bro.json "app" field
        }
    } else {
        // No arguments — auto-detect app from exe directory.
        // Also chdir there so that relative paths in bro.json ("app": "apps/…")
        // and engine-relative resolution (system/ panels, etc.) work when the
        // binary is launched from Finder / the macOS .app bundle (cwd = "/").
        std::string dir = exeDir();
#ifndef _WIN32
        if (!dir.empty() && dir != ".") chdir(dir.c_str());
#endif
        std::string configPath = dir + "/bro.json";

        if (fileExists(configPath)) {
            parseConfig(configPath, config);
            // Resolve relative app dir against exe directory
            if (config.appDir.empty()) {
                config.appDir = dir;
            } else if (config.appDir[0] != '/' && !(config.appDir.size() >= 2 && config.appDir[1] == ':')) {
                config.appDir = dir + "/" + config.appDir;
            }
        } else if (fileExists(dir + "/index.html")) {
            config.appDir = dir;
        } else {
            printUsage();
            return 1;
        }
    }

    // CLI splash overrides — applied last so they win over bro.json.
    if (cliNoSplash) config.showSplash = false;
    if (cliSplash)   config.showSplash = true;

    // Expose the resolved app directory to JS (process.env.BRO_APP_DIR) so the
    // launcher and other meta-apps can locate themselves and their siblings on
    // disk without guessing from cwd. Resolved to an absolute path.
    {
#ifdef _WIN32
        char abs[MAX_PATH];
        const char* appDirAbs = _fullpath(abs, config.appDir.c_str(), MAX_PATH)
                                ? abs : config.appDir.c_str();
        _putenv_s("BRO_APP_DIR", appDirAbs);
#else
        char abs[PATH_MAX];
        const char* appDirAbs = realpath(config.appDir.c_str(), abs)
                                ? abs : config.appDir.c_str();
        setenv("BRO_APP_DIR", appDirAbs, 1);
#endif
    }

    try {
        bro::engine::Engine engine(config);
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
