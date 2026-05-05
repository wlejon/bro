#pragma once

#include <vector>

#include <glad/gl.h>

#include "render/command_buffer.h"

namespace bro::canvas { class CanvasScene; }

namespace bro::engine {

/// One entry in the per-frame composite list. Built by the raster thread
/// when it scans the layout tree and breaks at canvas/WebGL/scene-graph
/// boundaries. Consumed by the main thread when it composites.
///
/// HTML layers point at a GPU-backed Skia surface allocated from one of the
/// raster thread's pools. Canvas layers point at a CanvasScene whose own
/// per-canvas thread produced the texture (the scene is queried for its
/// current texture id at composite time).
struct UILayer {
    enum Type { HTML, Canvas };
    Type type;
    GLuint texture = 0;
    canvas::CanvasScene* canvasScene = nullptr;
    float cx = 0, cy = 0, cw = 0, ch = 0;
};

/// Double-buffered slot. The main thread *writes* the command buffers (record
/// pass) before signaling raster. The raster thread *reads* the command
/// buffers and *writes* the layer lists (replay pass) before publishing the
/// fence. The two sides operate on the same slot in sequence, ordered by
/// the FrameWorker state machine.
///
/// App layers composite first (with the engine crosshair drawn between app
/// and system), then system layers on top so menu bar / preferences / splash
/// sit above app content + crosshair.
struct LayerBuffer {
    render::CommandBuffer  appCommands;
    render::CommandBuffer  systemCommands;
    std::vector<UILayer>   appLayers;
    std::vector<UILayer>   systemLayers;
};

} // namespace bro::engine
