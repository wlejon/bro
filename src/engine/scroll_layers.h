#pragma once

// A pass's scroll layers (layout/scroll_layer.h): the content the draw
// traversal records for each scroller composited as a layer of its own, the
// surface the raster thread draws its window into, and the picture the
// compositor places under the scroller's clip. The engine keeps one set for
// its frame passes and one for its headless captures (render::
// ScrollLayerSource::pass names which).
//
// Threads, as an iframe's sub-document: the main thread records into a layer
// (beginPass / beginScrollLayer / endPass) while the raster thread is idle;
// the raster thread replays (replay); the main thread composites what was
// published, whichever thread published it.

#include "engine/published_frame.h"
#include "layout/scroll_layer.h"
#include "render/command_buffer.h"
#include "render/skia_backend.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace bro::engine {

class ScrollLayerSet final : public layout::ScrollLayerHost {
public:
    explicit ScrollLayerSet(uint32_t pass) : pass_(pass) {}

    uint32_t scrollLayerPass() const override { return pass_; }
    render::CommandBuffer* beginScrollLayer(dom::Element* scroller, float winX, float winY, float winW,
                                            float winH, float originX, float originY) override;
    // A layer's device-pixel phase: where in its first pixel the window's
    // corner lands, for a quad origin at a render scale. The surface is
    // rastered with it and the compositor draws the image at the quad's
    // device origin less it, a whole pixel.
    static float phase(float originCss, float scale) {
        const float d = originCss * scale;
        return d - std::floor(d);
    }

    // Around a pass's record (main thread, raster idle): a layer not recorded
    // in the pass is dropped at its end.
    void beginPass();
    void endPass();
    // Raster thread: each layer recorded since it was last drawn, its window
    // into its surface, published.
    void replay(render::SkiaRenderer* renderer);
    // Compositing (main thread): what the scroller's layer last published, or null.
    const PublishedFrame* published(uint32_t elementId) const;
    // Every layer and its surfaces (the thread that draws them, or teardown).
    void clear() { layers_.clear(); }
    size_t size() const { return layers_.size(); }

private:
    struct Layer {
        render::CommandBuffer cmd;  // content space
        float winX = 0, winY = 0, winW = 0, winH = 0;
        float originX = 0, originY = 0;
        bool seen = false;
        bool needsReplay = true;
        render::LayerSurface surface, spare;
        int surfW = 0, surfH = 0;
        PublishedFrame published;
    };
    uint32_t pass_;
    std::unordered_map<uint32_t, std::unique_ptr<Layer>> layers_;
};

}  // namespace bro::engine
