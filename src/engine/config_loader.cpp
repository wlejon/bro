#include "engine/config_loader.h"
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

    float maxFps = getFloat("maxFps", 0);
    if (maxFps > 0) config.graphics.maxFrameIntervalMs = 1000.0 / maxFps;

    float scrollSpeed = getFloat("scrollSpeed", 0);
    if (scrollSpeed > 0) config.input.scrollSpeed = scrollSpeed;

    float dblClickTime = getFloat("doubleClickThreshold", 0);
    if (dblClickTime > 0) config.input.doubleClickThresholdMs = static_cast<double>(dblClickTime);

    float dblClickDist = getFloat("doubleClickDistance", 0);
    if (dblClickDist > 0) config.input.doubleClickDistancePx = dblClickDist;

    return true;
}

} // namespace bro::engine
