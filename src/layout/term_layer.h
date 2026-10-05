#pragma once

// TermLayer: a <terminal>'s own compositor layer, the part the engine sees.
//
// The element records its paint into `commands` (ElTerminal::recordLayer)
// on the main thread, only in the engine's raster-idle record block and only
// when something it shows changed. Whoever replays (the raster thread
// windowed, the main thread in a headless capture) draws `commands` into a
// surface of the box's size at the render scale and publishes the image the
// compositor places at the layer break DrawTraversal recorded for the
// element (render::TerminalLayerSource). A page change never re-records the
// terminal, and terminal output never re-records the page.
//
// Held by shared_ptr: a replay in flight keeps it valid after the element
// is gone.

#include "render/command_buffer.h"

#include <cstdint>

namespace bro::layout {

struct TermLayer {
    explicit TermLayer(uint64_t layerId) : id(layerId) {}
    const uint64_t id;
    render::CommandBuffer commands;  // the paint, at (0, 0) in CSS px
    int boxW = 0, boxH = 0;          // the content box, CSS px (rounded up)
    float scale = 1.0f;              // the render scale it was recorded for
    uint64_t generation = 0;         // bumped by every recording
};

} // namespace bro::layout
