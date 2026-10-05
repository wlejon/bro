#pragma once

#include "engine/published_frame.h"
#include "engine/replaced_elements.h"
#include "dom/document.h"
#include "canvas/canvas_scene.h"
#include "render/command_buffer.h"
#include "render/skia_backend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::engine {

/// An isolated sub-document hosted by an <iframe> element in the app document.
/// Owns its own DOM tree, canvas scenes, and input state.
struct IframeDoc {
    dom::Element* element = nullptr;  // the <iframe> in the app document (non-owning)
    uint64_t id = 0;                  // registry id for compositor texture resolve
    std::string src;                  // resolved src currently loaded (change detection)
    std::vector<std::unique_ptr<canvas::CanvasScene>> canvasScenes;
    std::unique_ptr<dom::Document> document;
    MouseDispatchState mouseState;    // per-doc click/dblclick tracking
    dom::Element* hoveredElement = nullptr; // sub-doc :hover target (non-owning)
    int boxW = 0, boxH = 0;           // last content-box size laid out
    render::CommandBuffer cmdBuffer;
    render::SkiaRenderer::LayerSurface surface;  // raster thread only
    int surfW = 0, surfH = 0;
    PublishedFrame published;                  // what the compositor draws
};

} // namespace bro::engine
