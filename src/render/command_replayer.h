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
// Maintains its own (font descriptor → renderer font handle) cache so we
// don't call dst_->createFont() per drawText. The cache lives across replay()
// invocations on the same instance — typical use is one CommandReplayer per
// raster thread, reused frame after frame.
class CommandReplayer {
public:
    using LayerBreakHandler = std::function<void(int kind, void* canvasScene,
                                                  unsigned int directTexture,
                                                  float x, float y, float w, float h)>;
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
    uint64_t resolveFont(std::string_view family, float size, int weight, bool italic);

    struct FontEntry {
        std::string family;
        float       size;
        int         weight;
        bool        italic;
        uint64_t    handle;
    };

    Renderer*               dst_;
    std::vector<FontEntry>  fontCache_;
    LayerBreakHandler       onLayerBreak_;
    BlitCanvasInlineHandler onBlitCanvasInline_;
};

} // namespace bro::render
