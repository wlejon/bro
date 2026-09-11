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
struct SystemDocument {
    std::string name;
    std::string tabLabel;
    std::string group;
    bool active = true;
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
    std::unique_ptr<dom::Document> document;
    MouseDispatchState mouseState;  // per-doc click/dblclick tracking
};

} // namespace bro::engine
