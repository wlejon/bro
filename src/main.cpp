#include "engine/engine.h"
#include "util/interrupt.h"
#include "util/log.h"
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif

// Get the directory containing the current executable.
static std::string exeDir() {
    std::string path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) path = std::string(buf, len);
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
        "  bro apps/dashboard\n"
        "\n"
        "bro.json format:\n"
        "  {\"app\": \".\", \"title\": \"My App\", \"width\": 1200, \"height\": 800}\n"
        "\n"
        "Additional bro.json options:\n"
        "  vsync (bool), resizable (bool), maxFps (number),\n"
        "  scrollSpeed (number), doubleClickThreshold (ms),\n"
        "  doubleClickDistance (px)\n"
        "\n"
        "See also: bro-headless for scripted/headless mode.\n");
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printUsage();
        return 0;
    }

    bro::util::installSignalHandler();

    bro::engine::EngineConfig config;

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

    if (argc >= 2) {
        // Explicit app directory argument
        config.appDir = argv[1];

        // Load bro.json from the app directory if present
        std::string appConfig = config.appDir + "/bro.json";
        if (fileExists(appConfig)) {
            parseConfig(appConfig, config);
            config.appDir = argv[1]; // preserve explicit appDir over bro.json "app" field
        }
    } else {
        // No arguments — try to auto-detect app from exe directory
        std::string dir = exeDir();
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

    try {
        bro::engine::Engine engine(config);
        engine.run();
    } catch (const std::exception& e) {
        LOG_ERROR("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
