#pragma once

#include "engine/engine.h"
#include <string>

namespace bro::engine {

/// Parse a bro.json file into an EngineConfig. Recognized keys:
///   App manifest:  app, title, width, height, vsync, resizable, splash,
///                  maxFps, scrollSpeed, doubleClickThreshold,
///                  doubleClickDistance, borderless, alwaysOnTop,
///                  minWidth, minHeight, maxWidth, maxHeight,
///                  windowX, windowY, display
///   Project root:  default_app, lib, system
///
/// Returns false if the file can't be opened. Sets `*outIsProjectManifest`
/// (when supplied) to true if any of the project-root keys was present —
/// callers use this to decide whether the bro.json's directory is a project
/// root or just an app dir with a manifest.
bool parseConfig(const std::string& path,
                 EngineConfig& config,
                 bool* outIsProjectManifest = nullptr);

/// Walk upward from `appDir` looking for the nearest ancestor directory whose
/// own bro.json is a project manifest (has default_app/lib/system keys).
/// Returns that ancestor's path, or "" if none is found within a few levels.
///
/// Covers launching an app by passing its own directory directly (e.g.
/// `bro-headless ../broworkshop/games/fps`) when that app's own bro.json
/// carries no project keys itself and BRO_PROJECT_ROOT isn't preset — without
/// this, apps that import shared `/lib/*` modules only work when spawned by
/// a parent bro process that already resolved the project root.
std::string findAncestorProjectRoot(const std::string& appDir);

} // namespace bro::engine
