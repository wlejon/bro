#include "engine/settings.h"
#include "engine/engine.h"  // for GraphicsConfig, InputConfig
#include "util/log.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bro::engine {

// ---------------------------------------------------------------------------
// JSON persistence helpers (same pattern as storage_bindings.cpp)
// ---------------------------------------------------------------------------

static std::string escapeJson(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\t': result += "\\t"; break;
            default:   result += c; break;
        }
    }
    return result;
}

static std::map<std::string, std::string> loadJson(const std::string& path) {
    std::map<std::string, std::string> result;
    std::ifstream file(path);
    if (!file.is_open()) return result;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    size_t pos = content.find('{');
    if (pos == std::string::npos) return result;
    pos++;

    auto parseString = [&](size_t& p) -> std::string {
        if (p >= content.size() || content[p] != '"') return "";
        p++;
        std::string r;
        while (p < content.size() && content[p] != '"') {
            if (content[p] == '\\' && p + 1 < content.size()) {
                p++;
                switch (content[p]) {
                    case '"':  r += '"'; break;
                    case '\\': r += '\\'; break;
                    case 'n':  r += '\n'; break;
                    case 't':  r += '\t'; break;
                    default:   r += content[p]; break;
                }
            } else {
                r += content[p];
            }
            p++;
        }
        if (p < content.size()) p++;
        return r;
    };

    while (pos < content.size()) {
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\n' ||
               content[pos] == '\r' || content[pos] == '\t' || content[pos] == ','))
            pos++;
        if (pos >= content.size() || content[pos] == '}') break;

        std::string key = parseString(pos);
        while (pos < content.size() && content[pos] != ':') pos++;
        if (pos < content.size()) pos++;
        while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t')) pos++;

        std::string value = parseString(pos);
        if (!key.empty()) result[key] = value;
    }
    return result;
}

static void saveJson(const std::string& path,
                     const std::map<std::string, std::string>& data) {
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to save settings to %s", path.c_str());
        return;
    }
    file << "{\n";
    bool first = true;
    for (auto& [key, val] : data) {
        if (!first) file << ",\n";
        file << "  \"" << escapeJson(key) << "\": \"" << escapeJson(val) << "\"";
        first = false;
    }
    file << "\n}\n";
}

// ---------------------------------------------------------------------------
// Construction / loading
// ---------------------------------------------------------------------------

Settings::Settings(const std::string& persistPath)
    : persistPath_(persistPath)
{
    // defaults_ already initialized by struct member initializers
    load();
    resolve();
}

void Settings::applyAppOverrides(const GraphicsConfig& gfx, const InputConfig& inp) {
    // Only mark fields as present if they differ from engine defaults,
    // since bro.json values flow through GraphicsConfig/InputConfig which
    // use the same defaults. We check each field individually.
    GraphicsSettings gfxDef;
    if (gfx.width != gfxDef.width) {
        appOverrides_.graphics.width = gfx.width;
        appPresence_.insert("graphics.width");
    }
    if (gfx.height != gfxDef.height) {
        appOverrides_.graphics.height = gfx.height;
        appPresence_.insert("graphics.height");
    }
    if (gfx.vsync != gfxDef.vsync) {
        appOverrides_.graphics.vsync = gfx.vsync;
        appPresence_.insert("graphics.vsync");
    }
    if (gfx.resizable != gfxDef.resizable) {
        appOverrides_.graphics.resizable = gfx.resizable;
        appPresence_.insert("graphics.resizable");
    }
    if (gfx.maxFrameIntervalMs != gfxDef.maxFrameIntervalMs) {
        appOverrides_.graphics.maxFrameIntervalMs = gfx.maxFrameIntervalMs;
        appPresence_.insert("graphics.maxFrameIntervalMs");
    }

    InputSettings inpDef;
    if (inp.scrollSpeed != inpDef.scrollSpeed) {
        appOverrides_.input.scrollSpeed = inp.scrollSpeed;
        appPresence_.insert("input.scrollSpeed");
    }
    if (inp.doubleClickThresholdMs != inpDef.doubleClickThresholdMs) {
        appOverrides_.input.doubleClickThresholdMs = inp.doubleClickThresholdMs;
        appPresence_.insert("input.doubleClickThresholdMs");
    }
    if (inp.doubleClickDistancePx != inpDef.doubleClickDistancePx) {
        appOverrides_.input.doubleClickDistancePx = inp.doubleClickDistancePx;
        appPresence_.insert("input.doubleClickDistancePx");
    }
    if (inp.overlayToggleKey != inpDef.overlayToggleKey) {
        appOverrides_.input.overlayToggleKey = inp.overlayToggleKey;
        appPresence_.insert("input.overlayToggleKey");
    }

    resolve();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void Settings::load() {
    if (persistPath_.empty()) return;

    auto data = loadJson(persistPath_);
    for (auto& [key, value] : data) {
        applyToLayer(userOverrides_, userPresence_, key, value);
    }
}

void Settings::save() {
    if (persistPath_.empty()) return;

    std::map<std::string, std::string> data;
    for (auto& key : userPresence_) {
        data[key] = getString(key);
    }

    // Also save action bindings
    for (auto& binding : userOverrides_.input.actionBindings) {
        std::string k = "input.bindings." + binding.action;
        std::string v;
        for (size_t i = 0; i < binding.keys.size(); i++) {
            if (i > 0) v += ",";
            v += binding.keys[i];
        }
        data[k] = v;
    }

    saveJson(persistPath_, data);
}

// ---------------------------------------------------------------------------
// Set user overrides
// ---------------------------------------------------------------------------

void Settings::setUser(const std::string& key, const std::string& value) {
    applyToLayer(userOverrides_, userPresence_, key, value);
    resolve();
    save();

    // Extract category from dotted key
    auto dot = key.find('.');
    std::string category = (dot != std::string::npos) ? key.substr(0, dot) : key;
    std::string field = (dot != std::string::npos) ? key.substr(dot + 1) : key;

    if (changeCallback_) changeCallback_(category, field);
}

void Settings::setUser(const std::string& key, double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    setUser(key, std::string(buf));
}

void Settings::setUser(const std::string& key, bool value) {
    setUser(key, std::string(value ? "true" : "false"));
}

void Settings::setUser(const std::string& key, int value) {
    setUser(key, std::to_string(value));
}

// ---------------------------------------------------------------------------
// Set app defaults
// ---------------------------------------------------------------------------

void Settings::setDefault(const std::string& key, const std::string& value) {
    applyToLayer(appOverrides_, appPresence_, key, value);
    resolve();
}

void Settings::setDefault(const std::string& key, double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    setDefault(key, std::string(buf));
}

void Settings::setDefault(const std::string& key, bool value) {
    setDefault(key, std::string(value ? "true" : "false"));
}

void Settings::setDefault(const std::string& key, int value) {
    setDefault(key, std::to_string(value));
}

// ---------------------------------------------------------------------------
// Get resolved value as string
// ---------------------------------------------------------------------------

std::string Settings::getString(const std::string& key) const {
    auto& g = resolved_.graphics;
    auto& a = resolved_.audio;
    auto& i = resolved_.input;

    if (key == "graphics.width") return std::to_string(g.width);
    if (key == "graphics.height") return std::to_string(g.height);
    if (key == "graphics.fullscreen") return g.fullscreen ? "true" : "false";
    if (key == "graphics.vsync") return g.vsync ? "true" : "false";
    if (key == "graphics.resizable") return g.resizable ? "true" : "false";
    if (key == "graphics.maxFrameIntervalMs") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", g.maxFrameIntervalMs);
        return buf;
    }

    if (key == "audio.masterVolume") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(a.masterVolume));
        return buf;
    }
    if (key == "audio.musicVolume") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(a.musicVolume));
        return buf;
    }
    if (key == "audio.sfxVolume") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(a.sfxVolume));
        return buf;
    }
    if (key == "audio.muted") return a.muted ? "true" : "false";

    if (key == "input.scrollSpeed") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(i.scrollSpeed));
        return buf;
    }
    if (key == "input.doubleClickThresholdMs") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", i.doubleClickThresholdMs);
        return buf;
    }
    if (key == "input.doubleClickDistancePx") {
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(i.doubleClickDistancePx));
        return buf;
    }
    if (key == "input.overlayToggleKey") return std::to_string(i.overlayToggleKey);

    return "";
}

// ---------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------

void Settings::resetCategory(const std::string& category) {
    std::string prefix = category + ".";
    std::vector<std::string> toRemove;
    for (auto& key : userPresence_) {
        if (key.substr(0, prefix.size()) == prefix) {
            toRemove.push_back(key);
        }
    }
    for (auto& key : toRemove) {
        userPresence_.erase(key);
    }

    if (category == "graphics") userOverrides_.graphics = GraphicsSettings{};
    else if (category == "audio") userOverrides_.audio = AudioSettings{};
    else if (category == "input") {
        // Preserve action bindings when resetting input settings
        auto bindings = std::move(userOverrides_.input.actionBindings);
        userOverrides_.input = InputSettings{};
        userOverrides_.input.actionBindings = std::move(bindings);
    }

    resolve();
    save();

    if (changeCallback_) changeCallback_(category, "*");
}

void Settings::resetAll() {
    userPresence_.clear();
    userOverrides_ = SettingsData{};
    resolve();
    save();

    if (changeCallback_) changeCallback_("*", "*");
}

// ---------------------------------------------------------------------------
// Action bindings
// ---------------------------------------------------------------------------

void Settings::defineEngineAction(const std::string& action,
                                  const std::vector<std::string>& defaultKeys) {
    // Add to defaults layer (lowest priority — apps and users can override)
    for (auto& b : defaults_.input.actionBindings) {
        if (b.action == action) {
            b.keys = defaultKeys;
            resolve();
            return;
        }
    }
    defaults_.input.actionBindings.push_back({action, defaultKeys});
    resolve();
}

void Settings::defineAction(const std::string& action,
                            const std::vector<std::string>& defaultKeys) {
    // Add to app layer
    for (auto& b : appOverrides_.input.actionBindings) {
        if (b.action == action) {
            b.keys = defaultKeys;
            resolve();
            return;
        }
    }
    appOverrides_.input.actionBindings.push_back({action, defaultKeys});
    appPresence_.insert("input.bindings." + action);
    resolve();
}

void Settings::rebindAction(const std::string& action,
                            const std::vector<std::string>& keys) {
    // Add to user layer
    for (auto& b : userOverrides_.input.actionBindings) {
        if (b.action == action) {
            b.keys = keys;
            resolve();
            save();
            return;
        }
    }
    userOverrides_.input.actionBindings.push_back({action, keys});
    resolve();
    save();
}

std::vector<std::string> Settings::getKeysForAction(const std::string& action) const {
    for (auto& b : resolved_.input.actionBindings) {
        if (b.action == action) return b.keys;
    }
    return {};
}

std::string Settings::getActionForKey(const std::string& webKey) const {
    auto it = keyToAction_.find(webKey);
    return (it != keyToAction_.end()) ? it->second : "";
}

// ---------------------------------------------------------------------------
// Layer resolution
// ---------------------------------------------------------------------------

void Settings::resolve() {
    resolveGraphics();
    resolveAudio();
    resolveInput();
    rebuildKeyToAction();
}

void Settings::resolveGraphics() {
    auto& r = resolved_.graphics;
    r = defaults_.graphics; // start from defaults

    if (appPresence_.count("graphics.width")) r.width = appOverrides_.graphics.width;
    if (appPresence_.count("graphics.height")) r.height = appOverrides_.graphics.height;
    if (appPresence_.count("graphics.fullscreen")) r.fullscreen = appOverrides_.graphics.fullscreen;
    if (appPresence_.count("graphics.vsync")) r.vsync = appOverrides_.graphics.vsync;
    if (appPresence_.count("graphics.resizable")) r.resizable = appOverrides_.graphics.resizable;
    if (appPresence_.count("graphics.maxFrameIntervalMs")) r.maxFrameIntervalMs = appOverrides_.graphics.maxFrameIntervalMs;

    if (userPresence_.count("graphics.width")) r.width = userOverrides_.graphics.width;
    if (userPresence_.count("graphics.height")) r.height = userOverrides_.graphics.height;
    if (userPresence_.count("graphics.fullscreen")) r.fullscreen = userOverrides_.graphics.fullscreen;
    if (userPresence_.count("graphics.vsync")) r.vsync = userOverrides_.graphics.vsync;
    if (userPresence_.count("graphics.resizable")) r.resizable = userOverrides_.graphics.resizable;
    if (userPresence_.count("graphics.maxFrameIntervalMs")) r.maxFrameIntervalMs = userOverrides_.graphics.maxFrameIntervalMs;
}

void Settings::resolveAudio() {
    auto& r = resolved_.audio;
    r = defaults_.audio;

    if (appPresence_.count("audio.masterVolume")) r.masterVolume = appOverrides_.audio.masterVolume;
    if (appPresence_.count("audio.musicVolume")) r.musicVolume = appOverrides_.audio.musicVolume;
    if (appPresence_.count("audio.sfxVolume")) r.sfxVolume = appOverrides_.audio.sfxVolume;
    if (appPresence_.count("audio.muted")) r.muted = appOverrides_.audio.muted;

    if (userPresence_.count("audio.masterVolume")) r.masterVolume = userOverrides_.audio.masterVolume;
    if (userPresence_.count("audio.musicVolume")) r.musicVolume = userOverrides_.audio.musicVolume;
    if (userPresence_.count("audio.sfxVolume")) r.sfxVolume = userOverrides_.audio.sfxVolume;
    if (userPresence_.count("audio.muted")) r.muted = userOverrides_.audio.muted;
}

void Settings::resolveInput() {
    auto& r = resolved_.input;
    r = defaults_.input;

    if (appPresence_.count("input.scrollSpeed")) r.scrollSpeed = appOverrides_.input.scrollSpeed;
    if (appPresence_.count("input.doubleClickThresholdMs")) r.doubleClickThresholdMs = appOverrides_.input.doubleClickThresholdMs;
    if (appPresence_.count("input.doubleClickDistancePx")) r.doubleClickDistancePx = appOverrides_.input.doubleClickDistancePx;
    if (appPresence_.count("input.overlayToggleKey")) r.overlayToggleKey = appOverrides_.input.overlayToggleKey;

    if (userPresence_.count("input.scrollSpeed")) r.scrollSpeed = userOverrides_.input.scrollSpeed;
    if (userPresence_.count("input.doubleClickThresholdMs")) r.doubleClickThresholdMs = userOverrides_.input.doubleClickThresholdMs;
    if (userPresence_.count("input.doubleClickDistancePx")) r.doubleClickDistancePx = userOverrides_.input.doubleClickDistancePx;
    if (userPresence_.count("input.overlayToggleKey")) r.overlayToggleKey = userOverrides_.input.overlayToggleKey;

    // Merge action bindings: engine defaults < app overrides < user overrides
    r.actionBindings = defaults_.input.actionBindings;

    // App layer overrides defaults, adds new actions
    for (auto& appBinding : appOverrides_.input.actionBindings) {
        bool found = false;
        for (auto& resolvedBinding : r.actionBindings) {
            if (resolvedBinding.action == appBinding.action) {
                resolvedBinding.keys = appBinding.keys;
                found = true;
                break;
            }
        }
        if (!found) {
            r.actionBindings.push_back(appBinding);
        }
    }

    // User layer overrides everything
    for (auto& userBinding : userOverrides_.input.actionBindings) {
        bool found = false;
        for (auto& resolvedBinding : r.actionBindings) {
            if (resolvedBinding.action == userBinding.action) {
                resolvedBinding.keys = userBinding.keys;
                found = true;
                break;
            }
        }
        if (!found) {
            r.actionBindings.push_back(userBinding);
        }
    }
}

void Settings::rebuildKeyToAction() {
    keyToAction_.clear();
    for (auto& binding : resolved_.input.actionBindings) {
        for (auto& key : binding.keys) {
            keyToAction_[key] = binding.action;
        }
    }
}

// ---------------------------------------------------------------------------
// Apply a key=value string to a settings layer
// ---------------------------------------------------------------------------

void Settings::applyToLayer(SettingsData& data, std::set<std::string>& presence,
                            const std::string& key, const std::string& value) {
    presence.insert(key);

    // Graphics
    if (key == "graphics.width") { data.graphics.width = std::stoi(value); return; }
    if (key == "graphics.height") { data.graphics.height = std::stoi(value); return; }
    if (key == "graphics.fullscreen") { data.graphics.fullscreen = (value == "true"); return; }
    if (key == "graphics.vsync") { data.graphics.vsync = (value == "true"); return; }
    if (key == "graphics.resizable") { data.graphics.resizable = (value == "true"); return; }
    if (key == "graphics.maxFrameIntervalMs") { data.graphics.maxFrameIntervalMs = std::stod(value); return; }

    // Audio
    if (key == "audio.masterVolume") { data.audio.masterVolume = std::stof(value); return; }
    if (key == "audio.musicVolume") { data.audio.musicVolume = std::stof(value); return; }
    if (key == "audio.sfxVolume") { data.audio.sfxVolume = std::stof(value); return; }
    if (key == "audio.muted") { data.audio.muted = (value == "true"); return; }

    // Input
    if (key == "input.scrollSpeed") { data.input.scrollSpeed = std::stof(value); return; }
    if (key == "input.doubleClickThresholdMs") { data.input.doubleClickThresholdMs = std::stod(value); return; }
    if (key == "input.doubleClickDistancePx") { data.input.doubleClickDistancePx = std::stof(value); return; }
    if (key == "input.overlayToggleKey") { data.input.overlayToggleKey = static_cast<uint32_t>(std::stoul(value)); return; }

    // Action bindings: input.bindings.<action> = "Key1,Key2"
    const std::string bindingsPrefix = "input.bindings.";
    if (key.substr(0, bindingsPrefix.size()) == bindingsPrefix) {
        std::string action = key.substr(bindingsPrefix.size());
        std::vector<std::string> keys;
        std::istringstream ss(value);
        std::string k;
        while (std::getline(ss, k, ',')) {
            if (!k.empty()) keys.push_back(k);
        }

        for (auto& b : data.input.actionBindings) {
            if (b.action == action) {
                b.keys = keys;
                return;
            }
        }
        data.input.actionBindings.push_back({action, keys});
        return;
    }

    // Unknown key — remove from presence since we didn't apply it
    presence.erase(key);
    LOG_WARN("Unknown settings key: %s", key.c_str());
}

} // namespace bro::engine
