#include "engine/config_loader.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bro::engine {

bool parseConfig(const std::string& path, EngineConfig& config,
                 bool* outIsProjectManifest)
{
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();

    auto getString = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos >= content.size() || content[pos] != '"') return {};
        pos++;
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

    auto getInt = [&](const char* key, int defaultVal) -> int {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return defaultVal;
        pos += needle.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos >= content.size()) return defaultVal;
        std::string num;
        if (content[pos] == '-') { num += '-'; pos++; }
        while (pos < content.size() && content[pos] >= '0' && content[pos] <= '9') {
            num += content[pos++];
        }
        if (num.empty() || num == "-") return defaultVal;
        return std::stoi(num);
    };

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

    auto getFloat = [&](const char* key, float defaultVal) -> float {
        std::string needle = std::string("\"") + key + "\"";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return defaultVal;
        pos += needle.size();
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r' || content[pos] == ':')) pos++;
        if (pos >= content.size()) return defaultVal;
        std::string num;
        if (content[pos] == '-') { num += '-'; pos++; }
        while (pos < content.size() && ((content[pos] >= '0' && content[pos] <= '9') || content[pos] == '.')) {
            num += content[pos++];
        }
        if (num.empty() || num == "-") return defaultVal;
        return std::stof(num);
    };

    std::string app = getString("app");
    if (!app.empty()) config.appDir = app;

    std::string title = getString("title");
    if (!title.empty()) config.title = title;

    // Project-root keys.
    std::string defaultApp = getString("default_app");
    if (!defaultApp.empty() && config.appDir.empty()) config.appDir = defaultApp;

    std::string libName = getString("lib");
    if (!libName.empty()) config.libDirName = libName;

    std::string systemName = getString("system");
    if (!systemName.empty()) config.systemDirName = systemName;

    if (outIsProjectManifest) {
        *outIsProjectManifest = !defaultApp.empty() || !libName.empty() || !systemName.empty();
    }

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

    // `"compiled": true` — this app dir's logic is an AOT-compiled program
    // linked into a host binary, not a script in the dir. Declaration only;
    // EngineConfig::compiledApp says what is done with it.
    int compiled = getBool("compiled");
    if (compiled >= 0) config.compiledApp = (compiled == 1);

    // Startup window management (runtime control lives in bro.window.*).
    int borderless = getBool("borderless");
    if (borderless >= 0) config.graphics.borderless = (borderless == 1);

    int alwaysOnTop = getBool("alwaysOnTop");
    if (alwaysOnTop >= 0) config.graphics.alwaysOnTop = (alwaysOnTop == 1);

    int minW = getInt("minWidth", 0);
    if (minW > 0) config.graphics.minWidth = minW;

    int minH = getInt("minHeight", 0);
    if (minH > 0) config.graphics.minHeight = minH;

    int maxW = getInt("maxWidth", 0);
    if (maxW > 0) config.graphics.maxWidth = maxW;

    int maxH = getInt("maxHeight", 0);
    if (maxH > 0) config.graphics.maxHeight = maxH;

    // Explicit position — negative values are legal on multi-monitor desktops,
    // so the "missing" default is the kWindowPosUnset sentinel, not 0/-1.
    config.graphics.windowX = getInt("windowX", config.graphics.windowX);
    config.graphics.windowY = getInt("windowY", config.graphics.windowY);

    int display = getInt("display", -1);
    if (display >= 0) config.graphics.display = display;

    float maxFps = getFloat("maxFps", 0);
    if (maxFps > 0) {
        config.graphics.maxFps = maxFps;              // present-rate cap
        config.graphics.maxFrameIntervalMs = 1000.0 / maxFps;  // + raster throttle
    }

    float scrollSpeed = getFloat("scrollSpeed", 0);
    if (scrollSpeed > 0) config.input.scrollSpeed = scrollSpeed;

    float dblClickTime = getFloat("doubleClickThreshold", 0);
    if (dblClickTime > 0) config.input.doubleClickThresholdMs = static_cast<double>(dblClickTime);

    float dblClickDist = getFloat("doubleClickDistance", 0);
    if (dblClickDist > 0) config.input.doubleClickDistancePx = dblClickDist;

    return true;
}

std::string findAncestorProjectRoot(const std::string& appDir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::absolute(appDir, ec);
    if (ec) return {};

    for (int depth = 0; depth < 8; ++depth) {
        fs::path parent = dir.parent_path();
        if (parent.empty() || parent == dir) break;

        std::string candidateJson = (parent / "bro.json").string();
        if (std::ifstream probe(candidateJson); probe.is_open()) {
            EngineConfig scratch;
            bool isProject = false;
            if (parseConfig(candidateJson, scratch, &isProject) && isProject) {
                return parent.string();
            }
        }
        dir = parent;
    }
    return {};
}

} // namespace bro::engine
