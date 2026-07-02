#pragma once

#include <vector>

#include <glad/gl.h>

#include "render/command_buffer.h"

namespace bro::engine {

/// One entry in the per-frame composite list. Built by the raster thread
/// when it scans the layout tree and breaks at canvas/WebGL/scene-graph
/// boundaries. Consumed by the main thread when it composites.
///
/// HTML layers point at a GPU-backed Skia surface allocated from one of the
/// raster thread's pools. Canvas layers name a CanvasScene by its never-
/// recycled sceneId; the main thread resolves the id through the engine's
/// scene registry at composite/signal time, so a layer recorded before the
/// scene was destroyed resolves to null instead of dangling (no scrub pass
/// over stale layer buffers needed).
struct UILayer {
    enum Type { HTML, Canvas };
    Type type;
    GLuint texture = 0;
    uint64_t canvasSceneId = 0;
    float cx = 0, cy = 0, cw = 0, ch = 0;
    // Overflow/scroll clip for Canvas layers, in top-left pixel space. The
    // canvas quad is composited outside the Skia clip stack, so the compositor
    // scissors to this rect. clipW < 0 ⇒ unclipped.
    float clipX = 0, clipY = 0, clipW = -1, clipH = -1;
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
