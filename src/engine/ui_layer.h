#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "render/command_buffer.h"
#include "render/layer_source.h"
#include "render/skia_gpu.h"

#include <include/core/SkSurface.h>
#include <vulkan/vulkan.h>

namespace bro::engine {

/// An HTML layer: a Skia surface from one of the raster thread's pools — its
/// GPU image (`image`) when Skia draws on the GPU, else the CPU surface. The
/// layer covers the whole pass (content space for the app document).
struct HtmlLayer {
    sk_sp<SkSurface> surface;    // CPU
    render::SkiaImageRef image;  // GPU
};

/// One entry in the per-frame composite list, built by the raster thread as
/// it replays a pass and breaks the HTML around separately composited
/// content; consumed by the main thread when it composites. A non-HTML
/// layer names its content by handle (render::LayerSource), resolved through
/// the engine's registries at composite time, so a layer that outlives what
/// it names draws nothing rather than dangling.
struct UILayer {
    using Content = std::variant<HtmlLayer, render::CanvasLayerSource, render::WebGLLayerSource,
                                 render::SceneLayerSource, render::IframeLayerSource,
                                 render::TerminalLayerSource, render::DmabufLayerSource>;
    Content content;
    render::LayerQuad quad;  // non-HTML layers

    static UILayer of(const render::LayerSource& source, const render::LayerQuad& quad) {
        UILayer layer;
        layer.content = std::visit([](const auto& s) { return Content{s}; }, source);
        layer.quad = quad;
        return layer;
    }
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
