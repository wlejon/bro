#pragma once

#include "engine/replaced_elements.h"
#include "dom/document.h"
#include "canvas/canvas_scene.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::engine {

/// System panels are ordinary HTML documents rendered through the same
/// layout/raster pipeline as the app document. They share the engine's
/// textMetrics_ for layout, and in windowed mode the raster thread draws
/// each visible panel into its own GPU surface.
///
/// A panel is found at startup but built (parsed, laid out, its scripts run)
/// only the first time it is shown: most windows never open the inspector or
/// the settings, and building all of them cost every app ~250 ms before its
/// first frame. `document` stays null until then.
struct SystemDocument {
    std::string name;
    std::string tabLabel;
    std::string group;
    std::string htmlPath;   // the panel's .html
    std::string dirPath;    // its directory (scripts and assets resolve here)
    bool loaded = false;    // built (or tried to be: an unreadable file stays unloaded)
    bool active = true;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
    std::unique_ptr<dom::Document> document;
    MouseDispatchState mouseState;  // per-doc click/dblclick tracking
};

} // namespace bro::engine
