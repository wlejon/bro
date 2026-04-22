#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::engine {

// Forward declarations for applyAppOverrides
struct GraphicsConfig;
struct InputConfig;

// ---------------------------------------------------------------------------
// Settings data structs
// ---------------------------------------------------------------------------

struct GraphicsSettings {
    int width = 1920;
    int height = 1080;
    bool fullscreen = false;
    bool vsync = true;
    bool resizable = true;
    double maxFrameIntervalMs = 8.0;
};

struct AudioSettings {
    float masterVolume = 1.0f;
    float musicVolume = 1.0f;
    float sfxVolume = 1.0f;
    bool muted = false;
};

struct ActionBinding {
    std::string action;
    std::vector<std::string> keys;
};

struct InputSettings {
    float scrollSpeed = 48.0f;
    double doubleClickThresholdMs = 500.0;
    float doubleClickDistancePx = 5.0f;
    uint32_t overlayToggleKey = 0x40000041u; // SDLK_F8
    std::vector<ActionBinding> actionBindings;
};

struct SettingsData {
    GraphicsSettings graphics;
    AudioSettings audio;
    InputSettings input;
};

// ---------------------------------------------------------------------------
// Settings manager — three-layer priority system
//   engine defaults < app overrides (bro.json) < user overrides (persisted)
// ---------------------------------------------------------------------------

class Settings {
public:
    explicit Settings(const std::string& persistPath);

    /// Apply app-level overrides from bro.json config. Called once at init.
    void applyAppOverrides(const GraphicsConfig& gfx, const InputConfig& inp);

    // --- Resolved (final) values ---
    const SettingsData& current() const { return resolved_; }
    const GraphicsSettings& graphics() const { return resolved_.graphics; }
    const GraphicsSettings& graphicsDefaults() const { return defaults_.graphics; }
    const AudioSettings& audio() const { return resolved_.audio; }
    const InputSettings& input() const { return resolved_.input; }

    // --- User overrides (persisted) ---
    void setUser(const std::string& key, const std::string& value);
    void setUser(const std::string& key, double value);
    void setUser(const std::string& key, bool value);
    void setUser(const std::string& key, int value);

    // --- App defaults (non-persisted) ---
    void setDefault(const std::string& key, const std::string& value);
    void setDefault(const std::string& key, double value);
    void setDefault(const std::string& key, bool value);
    void setDefault(const std::string& key, int value);

    /// Get a setting value as string (for JS API).
    std::string getString(const std::string& key) const;

    // --- Reset ---
    void resetCategory(const std::string& category);
    void resetAll();

    // --- Action bindings ---
    void defineEngineAction(const std::string& action, const std::vector<std::string>& defaultKeys);
    void defineAction(const std::string& action, const std::vector<std::string>& defaultKeys);
    void rebindAction(const std::string& action, const std::vector<std::string>& keys);
    /// Remove the user-level rebind for a single action, reverting to
    /// app/engine defaults. Other action overrides are left intact.
    void resetAction(const std::string& action);
    /// Remove all user-level rebinds for actions at once.
    void resetAllActions();
    std::vector<std::string> getKeysForAction(const std::string& action) const;
    std::string getActionForKey(const std::string& webKey) const;
    const std::vector<ActionBinding>& getActions() const { return resolved_.input.actionBindings; }

    // --- Persistence ---
    void save();
    void load();

    // --- Change notification ---
    using ChangeCallback = std::function<void(const std::string& category,
                                              const std::string& key)>;
    void setChangeCallback(ChangeCallback cb) { changeCallback_ = std::move(cb); }

private:
    void resolve();
    void resolveGraphics();
    void resolveAudio();
    void resolveInput();
    void rebuildKeyToAction();

    // Apply a key=value into a SettingsData + mark presence
    void applyToLayer(SettingsData& data, std::set<std::string>& presence,
                      const std::string& key, const std::string& value);

    // Three layers
    SettingsData defaults_;
    SettingsData appOverrides_;
    SettingsData userOverrides_;
    SettingsData resolved_;

    // Track which fields have been explicitly set
    std::set<std::string> appPresence_;
    std::set<std::string> userPresence_;

    // Action binding reverse lookup: web key -> action name
    std::unordered_map<std::string, std::string> keyToAction_;

    std::string persistPath_;
    ChangeCallback changeCallback_;
};

} // namespace bro::engine
