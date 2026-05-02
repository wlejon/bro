#pragma once

#include "engine/engine.h"
#include <string>

namespace bro::engine {

/// Parse a bro.json file into an EngineConfig. Recognized keys:
///   App manifest:  app, title, width, height, vsync, resizable, splash,
///                  maxFps, scrollSpeed, doubleClickThreshold,
///                  doubleClickDistance
///   Project root:  default_app, lib, system
///
/// Returns false if the file can't be opened. Sets `*outIsProjectManifest`
/// (when supplied) to true if any of the project-root keys was present —
/// callers use this to decide whether the bro.json's directory is a project
/// root or just an app dir with a manifest.
bool parseConfig(const std::string& path,
                 EngineConfig& config,
                 bool* outIsProjectManifest = nullptr);

} // namespace bro::engine
