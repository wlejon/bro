#pragma once

#include "render/command_buffer.h"
#include "render/renderer.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bro::render {

// Replay a CommandBuffer against a live `Renderer`. Layer-break commands are
// not handed to the renderer — they are passed to a callback so the engine
// compositor can flush the current Skia surface, push a Canvas/WebGL UILayer,
// and start a fresh surface for subsequent HTML commands.
//
// Each Cmd_DrawText embeds the full font descriptor (family/size/weight/
// italic). Replay reads that descriptor straight back into a FontRef and
// hands it to `dst->drawText`; the destination renderer caches by content
// internally, so no replayer-side font cache is needed.
class CommandReplayer {
public:
    using LayerBreakHandler = std::function<void(int kind, uint64_t canvasSceneId,
                                                  unsigned int directTexture,
                                                  float x, float y, float w, float h,
                                                  float clipX, float clipY,
                                                  float clipW, float clipH)>;
    using BlitCanvasInlineHandler = std::function<void(void* canvasScene,
                                                        float x, float y, float w, float h)>;

    explicit CommandReplayer(Renderer* dst);

    void setLayerBreakHandler(LayerBreakHandler h) { onLayerBreak_ = std::move(h); }
    void setBlitCanvasInlineHandler(BlitCanvasInlineHandler h) {
        onBlitCanvasInline_ = std::move(h);
    }

    // Replay every command in the buffer in order against `dst`.
    void replay(const CommandBuffer& buffer);

private:
    // A layer-break hands `dst_` a brand-new surface (fresh matrix/clip
    // state) partway through replay — see openStack_ below.
    struct StackOp {
        enum Kind { Concat, Concat4x4, Translate, Scale, Rotate,
                    SetClip, SetClipRRect, SetClipPolygon, ResetClip } kind;
        float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0; // Concat
        float m[16] = {};                               // Concat4x4
        float x = 0, y = 0, w = 0, h = 0;                // Translate/Scale/SetClip(RRect)
        float degrees = 0;                               // Rotate
        Radii radii{};                                    // SetClipRRect
        std::vector<PointF> points;                       // SetClipPolygon
    };
    struct OpenFrame {
        enum SaveKind { Plain, LayerAlpha, LayerFilter, LayerBlend } saveKind = Plain;
        uint8_t alpha = 255;                              // LayerAlpha
        std::vector<CssFilterParams> filters;              // LayerFilter
        float flx = 0, fly = 0, flw = 0, flh = 0;          // LayerFilter bounds
        BlendMode blendMode{};                             // LayerBlend
        std::vector<StackOp> ops;
    };

    // Every Save/SaveLayer* not yet matched by a Restore, oldest-first, with
    // the transform/clip mutations issued at each level. A layer-break
    // switches `dst_` to a fresh surface with identity matrix and no clip —
    // replaying this stack onto it reconstructs the state the original
    // (single continuous) canvas would have had, so an ancestor CSS
    // transform/clip stays active for HTML content painted after the break.
    void replayOpenStackOnto(Renderer* dst);

    std::vector<OpenFrame>  openStack_;

    Renderer*               dst_;
    LayerBreakHandler       onLayerBreak_;
    BlitCanvasInlineHandler onBlitCanvasInline_;
};

} // namespace bro::render
