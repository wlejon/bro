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
    enum Type { HTML, Canvas, Iframe };
    Type type;
    GLuint texture = 0;
    // CanvasScene id when type==Canvas; IframeDoc id when type==Iframe. Both are
    // resolved through an engine registry at composite time so a layer that
    // outlives its scene/sub-document draws nothing rather than dangling.
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
/// App layers composite first, then system layers on top so menu bar /
/// preferences / splash sit above app content.
struct LayerBuffer {
    // Base app commands live in the engine's single cross-frame cache
    // (Engine::baseCommands_), not here — a per-slot copy can't survive the
    // front/back ping-pong (the back slot is two frames stale). This slot only
    // carries the per-frame-fresh promoted + system commands and the output
    // layer lists.
    render::CommandBuffer  systemCommands;
    // Promoted (compositor-layer) subtree commands, recorded fresh every frame
    // even when the cached base is reused. Replayed after the base into one
    // extra surface and composited on top. Empty when nothing is promoted.
    render::CommandBuffer  promotedCommands;
    std::vector<UILayer>   appLayers;
    std::vector<UILayer>   systemLayers;

    // Composite-time placement for the app layer set. App layers are
    // content-sized and recorded in content space; the compositor draws them
    // at (0, appInsetTop) with appContentW × appContentH. Written by the main
    // thread at record time (alongside appCommands) so a claimed frame always
    // composites with the insets it was recorded under — even if the live
    // insets have changed since (menu shown/hidden mid-flight). System layers
    // are always full-viewport at (0, 0) and need no placement here.
    int appInsetTop = 0;
    int appContentW = 0;
    int appContentH = 0;
};

} // namespace bro::engine
