#pragma once

#include <cstdint>
#include <variant>

namespace bro::render {

// What a separately composited layer shows, named by a typed handle rather
// than a pointer: a recorded layer can outlive what it names (the frame it was
// recorded in is replayed and composited later, on other threads), so the
// compositor resolves each handle through the engine's registries at use time
// and a stale one resolves to nothing instead of dangling.

/// A 2D canvas: CanvasScene::sceneId() (never recycled).
struct CanvasLayerSource { uint64_t sceneId = 0; };
/// A WebGL canvas: the <canvas> element's node id.
struct WebGLLayerSource { uint32_t elementId = 0; };
/// A 3D scene's output image: the scene element's node id.
struct SceneLayerSource { uint32_t elementId = 0; };
/// An <iframe> sub-document: its IframeDoc id (never recycled).
struct IframeLayerSource { uint64_t docId = 0; };

/// A <terminal>'s screen: its ElTerminal::layerId() (never recycled).
struct TerminalLayerSource { uint64_t layerId = 0; };

/// A DMA-BUF client buffer: foreign toplevel or client surface imported via brodmabuf
struct DmabufLayerSource {
    uint64_t bufferId = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t strides[4] = {0, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint64_t modifier = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t drmFormat = 0;
    uint32_t planeCount = 1;
    int syncFd = -1;  // explicit sync fence fd (<0 means none/implicit)
};

/// Placeholder layer break for client windows in shell applications
struct ClientWindowsLayerSource {};

using LayerSource = std::variant<CanvasLayerSource, WebGLLayerSource, SceneLayerSource, IframeLayerSource,
                                 TerminalLayerSource, DmabufLayerSource, ClientWindowsLayerSource>;

/// Where a layer lands, in the surface space of the HTML painted around it
/// (CSS px; content space for the app document), and the overflow/scroll
/// clip active where it was reached. The layer is composited outside the
/// Skia clip stack, so the compositor re-applies the clip itself.
struct LayerQuad {
    float x = 0, y = 0, w = 0, h = 0;
    float clipX = 0, clipY = 0, clipW = -1, clipH = -1;  // clipW < 0: unclipped

    bool clipped() const { return clipW >= 0.0f && clipH >= 0.0f; }
};

} // namespace bro::render
