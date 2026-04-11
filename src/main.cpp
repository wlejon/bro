#include "engine/engine.h"
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

    std::string app = getString("app");
    if (!app.empty()) config.appDir = app;

    std::string title = getString("title");
    if (!title.empty()) config.title = title;

    int w = getInt("width", 0);
    if (w > 0) config.width = w;

    int h = getInt("height", 0);
    if (h > 0) config.height = h;

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
        "See also: bro-headless for scripted/headless mode.\n");
}

int main(int argc, char* argv[]) {
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        printUsage();
        return 0;
    }

    bro::engine::EngineConfig config;

    if (argc >= 2) {
        // Explicit app directory argument
        config.appDir = argv[1];
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
