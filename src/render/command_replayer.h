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
    using LayerBreakHandler = std::function<void(int kind, void* canvasScene,
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
    Renderer*               dst_;
    LayerBreakHandler       onLayerBreak_;
    BlitCanvasInlineHandler onBlitCanvasInline_;
};

} // namespace bro::render
