#pragma once

#include <string>
#include <vector>

namespace bro::platform { class Window; }

namespace bro::platform::desktop {

struct TrayMenuItem {
    std::string id;
    std::string label;
    std::string type; // "normal", "separator", "checkbox"
    bool checked = false;
    bool enabled = true;
};

struct TrayConfig {
    std::string icon;
    std::string tooltip;
    std::vector<TrayMenuItem> menu;
};

/// Sets or updates the system tray icon and menu.
bool setTray(const Window* window, const TrayConfig& config);

/// Removes the tray icon.
bool removeTray();

/// Whether a tray icon is currently active.
bool hasTray();

/// Whether system tray is supported on this platform.
bool isTrayAvailable();

/// Headless inspection and simulation.
const TrayConfig& getRecordedTray();

} // namespace bro::platform::desktop
