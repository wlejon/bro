#pragma once

#include "engine/scrollbar.h"
#include <climits>
#include <cstdint>
#include <functional>
#include <string>

struct JSContext;

namespace bro::engine {

enum class DisplayMode { Windowed, Headless, Server };

/// Sentinel for "no explicit startup window position requested" (the bro.json
/// windowX/windowY keys). Any real coordinate — including negative ones on a
/// multi-monitor desktop — is representable, so use INT_MIN, not 0/-1.
inline constexpr int kWindowPosUnset = INT_MIN;

/// Graphics/display settings configurable per app.
struct GraphicsConfig {
    int width = 1920;
    int height = 1080;
    bool useGPU = true;       // headless uses GPU by default; --no-gpu disables
    bool resizable = true;    // whether the window can be resized
    bool vsync = true;        // true = adaptive or standard vsync; false = uncapped
    double maxFrameIntervalMs = 8.0;  // layout/raster throttle (0 = uncapped)
    double maxFps = 0.0;      // present-rate cap independent of vsync (0 = uncapped)

    // Startup window management
    bool borderless = false;   // no title bar / border (SDL_WINDOW_BORDERLESS)
    bool alwaysOnTop = false;  // keep above all normal windows
    int minWidth = 0;          // min/max resize limits; 0 = unconstrained
    int minHeight = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    int windowX = kWindowPosUnset;  // explicit startup position (both must be set)
    int windowY = kWindowPosUnset;
    int display = -1;          // display INDEX to center on at startup; -1 = OS default
};

/// Input behavior settings configurable per app.
struct InputConfig {
    float scrollSpeed = 48.0f;             // pixels per mouse wheel tick
    double doubleClickThresholdMs = 500.0; // max time between clicks for dblclick
    float doubleClickDistancePx = 5.0f;    // max movement between clicks for dblclick
    uint32_t overlayToggleKey = 0x40000041u; // SDLK_F8; 0 = disabled
};

struct EngineConfig {
    std::string appDir;
    std::string title;   // window title override (empty = use <title> from HTML)
    std::string settingsPath; // path to .bro_settings.json (empty = auto-detect)
    std::string projectRoot;
    std::string libDirName    = "lib";
    std::string systemDirName = "system";
    DisplayMode displayMode = DisplayMode::Windowed;
    bool realAudio = false;
    bool showSplash = true;
    bool compiledApp = false;
    bool hostProvidesCompiledApp = false;
    std::function<void(JSContext*)> installHostBindings;
    GraphicsConfig graphics;
    InputConfig input;
    Scrollbar::Style viewportScrollbar;
    Scrollbar::Style elementScrollbar{5.0f, 1.0f, 16.0f,
        {255,255,255,20}, {255,255,255,100}, {255,255,255,150}, {255,255,255,180}};
};

} // namespace bro::engine
