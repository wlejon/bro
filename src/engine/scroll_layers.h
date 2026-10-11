#pragma once

// A pass's scroll layers (layout/scroll_layer.h): the content the draw
// traversal records for each scroller composited as a layer of its own, the
// surface the raster thread draws its window into, and the picture the
// compositor places under the scroller's clip. The engine keeps one set for
// its frame passes and one for its headless captures (render::
// ScrollLayerSource::pass names which).
//
// A layer's recording stands from pass to pass while nothing under its
// scroller is stamped and its window still holds the view; its surface is
// rastered again only when that recording changes or a scroll moves it by a
// fraction of a device pixel (or the scale changes). The compositor places
// the picture by the scroll of the pass it was published for.
//
// Threads, as an iframe's sub-document: the main thread records into a layer
// (beginPass / beginScrollLayer / endPass) while the raster thread is idle;
// the raster thread replays (replay) and publishes what it drew with where it
// drew it (Shown); the main thread composites what was published, whichever
// thread published it.

#include "engine/published_frame.h"
#include "layout/scroll_layer.h"
#include "render/command_buffer.h"
#include "render/skia_backend.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace bro::engine {

class ScrollLayerSet final : public layout::ScrollLayerHost {
public:
    explicit ScrollLayerSet(uint32_t pass) : pass_(pass) {}

    uint32_t scrollLayerPass() const override { return pass_; }
    layout::ScrollLayerRecord beginScrollLayer(dom::Element* scroller, const layout::ScrollLayerPlace& place,
                                               bool mayReuse) override;
    void endScrollLayer(dom::Element* scroller, bool reusable) override;
    // A layer's device-pixel phase: where in its first pixel the window's
    // corner lands, for an origin at a render scale. The surface is rastered
    // with it and the compositor draws the image at the origin's device
    // place less it, a whole pixel.
    static float phase(float originCss, float scale) {
        const float d = originCss * scale;
        return d - std::floor(d);
    }
    // How far round the view a recorded window reaches (CSS px).
    static constexpr float kMargin = 256.0f;

    // Around a pass's record (main thread, raster idle): a layer not recorded
    // in the pass is dropped at its end.
    void beginPass();
    void endPass();
    // Raster thread: each layer whose recording, phase or scale changed, its
    // window into its surface; every layer's Shown published.
    void replay(render::SkiaRenderer* renderer);

    // What the compositor draws for a scroller: the picture, the window it
    // holds (content space) and the phase it was rastered at, and the scroll
    // to place it by.
    struct Shown {
        sk_sp<SkImage> image;              // a CPU picture
        render::SkiaImageRef gpu;          // or a GPU one
        uint64_t generation = 0;           // bumped by each picture published
        float winX = 0, winY = 0;
        float phaseX = 0, phaseY = 0;
        float scrollX = 0, scrollY = 0;
    };
    // Compositing (main thread): false when the scroller has no layer or
    // nothing was published for it yet.
    bool shown(uint32_t elementId, Shown& out) const;

    // Every layer and its surfaces (the thread that draws them, or teardown).
    void clear() { layers_.clear(); }
    size_t size() const { return layers_.size(); }
    // Layer surfaces rastered, since startup.
    uint64_t rasters() const { return rasters_.load(std::memory_order_relaxed); }

private:
    struct Layer {
        // Recorded by the main thread, read by the raster thread's replay.
        render::CommandBuffer cmd;  // content space
        float winX = 0, winY = 0, winW = 0, winH = 0;
        float contentX = 0, contentY = 0;
        float scrollX = 0, scrollY = 0;  // the pass's scroll
        uint64_t epoch = 0;              // the paint epoch it was recorded in
        bool reusable = false;
        bool seen = false;
        bool recorded = true;            // recorded since it was last rastered

        // The raster thread's: its surfaces and what it last drew in them.
        render::LayerSurface surface, spare;
        int surfW = 0, surfH = 0;
        float rasterWinX = 0, rasterWinY = 0, rasterPhaseX = -1, rasterPhaseY = -1, rasterScale = 0;

        // Published by the raster thread, read by the compositor.
        mutable std::mutex shownMutex;
        Shown shown;
        bool published = false;
    };
    uint32_t pass_;
    std::unordered_map<uint32_t, std::unique_ptr<Layer>> layers_;
    std::atomic<uint64_t> rasters_{0};
};

}  // namespace bro::engine
