#pragma once

// A pass's scroll layers (layout/scroll_layer.h): the content the draw
// traversal records for each scroller composited as a layer of its own, the
// surface the raster thread draws its window into, and the picture the
// compositor places under the scroller's clip. The engine keeps one set for
// its frame passes and one for its headless captures (render::
// ScrollLayerSource::pass names which).
//
// A layer is parts, each a recording of its own over one window: its content
// (the scroller's in-flow descendants, painted where the scroller is) and
// pieces (runs of its positioned descendants and stacking contexts, painted
// where the stacking-context walk puts them among the page's), each placed
// where its layer break is in the page, all moved by the one scroll.
//
// A layer's recordings stand from pass to pass while nothing under its
// scroller is stamped and its window still holds the view; its surfaces are
// rastered again only when a recording changes or a scroll moves them by a
// fraction of a device pixel (or the scale changes). The compositor places
// the pictures by the scroll of the pass they were published for, or, once
// the pictures of the latest pass are out, by the layer's live scroll: what
// the main thread moved it to since (scrollTo), with no pass at all, when the
// window holds the view there and the move is by whole device pixels. Its
// scrollbar thumbs are pictures of their own the compositor moves the same
// way (setThumb); the page paints only their tracks.
//
// Threads, as an iframe's sub-document: the main thread records into a layer
// (beginPass / beginScrollLayer / beginScrollPiece / endPass) while the
// raster thread is idle; the raster thread replays (replay) and publishes
// what it drew with where it drew it (Shown); the main thread composites what
// was published, whichever thread published it.

#include "engine/published_frame.h"
#include "layout/scroll_layer.h"
#include "render/command_buffer.h"
#include "render/skia_backend.h"

#include <bromath/color.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bro::engine {

class ScrollLayerSet final : public layout::ScrollLayerHost {
public:
    explicit ScrollLayerSet(uint32_t pass) : pass_(pass) {}

    uint32_t scrollLayerPass() const override { return pass_; }
    layout::ScrollLayerRecord beginScrollLayer(dom::Element* scroller, const layout::ScrollLayerPlace& place,
                                               bool mayReuse) override;
    void endScrollLayer(dom::Element* scroller, bool reusable) override;
    layout::ScrollLayerRecord beginScrollPiece(dom::Element* scroller, uint32_t index,
                                               const std::vector<uint32_t>& entries, bool mayReuse) override;
    void endScrollPiece(dom::Element* scroller, uint32_t index, bool reusable) override;
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
    // in the pass is dropped at its end, and so is a piece past the ones it
    // recorded.
    void beginPass();
    void endPass();
    // A scrollbar thumb of a layer's scroller, for the pass being recorded
    // (main thread): `axis` 0 vertical, 1 horizontal. Its quad (the layer
    // break the caller records) is the thumb at scroll 0; along its axis it
    // starts at the track's start + scroll / scrollRange * thumbRange, at a
    // whole device pixel (Scrollbar::snapThumb), as painting it in place puts it.
    void setThumb(uint32_t elementId, int axis, float w, float h, float crossPos, float scrollRange,
                  float thumbRange, bromath::Color color);
    // Whether a layer's scroller has a scroll container under it (whose bars
    // the page paints where this scroller's scroll puts them).
    void setHasNestedScroller(uint32_t elementId);

    // Move a layer's pictures to a scroll with no pass (main thread): true
    // when its latest recordings stand there (nothing under the scroller
    // stamped, nothing volatile in them, no scroller in it), its window holds
    // the view there, and the move from the pass's scroll is by whole device
    // pixels at `scale`. False: the page has to be recorded for it.
    bool scrollTo(dom::Element* scroller, float scrollX, float scrollY, float scale);

    // Raster thread: each part whose recording changed, or all of a layer's
    // when its phase or scale did, its window into its surface, and its
    // thumbs; every layer's Shown published.
    void replay(render::SkiaRenderer* renderer);

    // What the compositor draws for a scroller: a picture per part (null for
    // a part that paints nothing), the window they hold (content space) and
    // the phase they were rastered at, the scroll to place them by, and its
    // thumbs.
    struct Picture {
        sk_sp<SkImage> image;              // a CPU picture
        render::SkiaImageRef gpu;          // or a GPU one
    };
    struct ThumbShown {
        Picture picture;
        float phase = 0;  // across its axis
        float scrollRange = 0, thumbRange = 0;
    };
    struct Shown {
        std::vector<Picture> parts;        // [0] the content, then the pieces
        uint64_t generation = 0;           // bumped by each picture published
        float winX = 0, winY = 0;
        float phaseX = 0, phaseY = 0;
        float scrollX = 0, scrollY = 0;
        ThumbShown thumbs[2];
    };
    // Compositing (main thread): false when the scroller has no layer or
    // nothing was published for it yet.
    bool shown(uint32_t elementId, Shown& out) const;

    // Every layer and its surfaces (the thread that draws them, or teardown).
    void clear() { layers_.clear(); }
    // Whether the pass being recorded made the scroller a layer.
    bool placed(uint32_t elementId) const {
        auto it = layers_.find(elementId);
        return it != layers_.end() && it->second->seen;
    }
    size_t size() const { return layers_.size(); }
    // Layer surfaces rastered (a part each), since startup.
    uint64_t rasters() const { return rasters_.load(std::memory_order_relaxed); }

private:
    // One recording of a layer and the surfaces it is rastered into.
    struct Part {
        // Recorded by the main thread, read by the raster thread's replay.
        render::CommandBuffer cmd;  // content space
        std::vector<uint32_t> entries;  // a piece's: the elements whose paint it holds
        bool reusable = false;
        bool recorded = true;  // recorded since it was last rastered
        // The raster thread's.
        render::LayerSurface surface, spare;
        int surfW = 0, surfH = 0;
    };
    struct Layer {
        // Recorded by the main thread, read by the raster thread's replay.
        std::vector<std::unique_ptr<Part>> parts;  // [0] the content
        size_t partsThisPass = 0;
        float winX = 0, winY = 0, winW = 0, winH = 0;
        float contentX = 0, contentY = 0;
        float scrollX = 0, scrollY = 0;  // the pass's scroll
        float viewOffX = 0, viewOffY = 0, viewW = 0, viewH = 0;  // the view at scroll 0
        uint64_t epoch = 0;              // the paint epoch its content was recorded in
        bool nestedScroller = false;
        bool seen = false;
        uint64_t passes = 0;             // passes that placed it
        struct Thumb {
            bool on = false;
            float w = 0, h = 0, crossPos = 0, scrollRange = 0, thumbRange = 0;
            bromath::Color color;
            bool operator==(const Thumb& o) const {
                return on == o.on && w == o.w && h == o.h && crossPos == o.crossPos &&
                       color.r == o.color.r && color.g == o.color.g && color.b == o.color.b &&
                       color.a == o.color.a;
            }
        };
        Thumb thumbs[2];
        // Where the main thread scrolled it: the pass's scroll, then scrollTo's.
        std::atomic<float> liveX{0}, liveY{0};

        // The raster thread's: what it last drew the parts at, and the thumbs.
        float rasterWinX = 0, rasterWinY = 0, rasterPhaseX = -1, rasterPhaseY = -1, rasterScale = 0;
        render::LayerSurface thumbSurface[2], thumbSpare[2];
        Thumb rasterThumb[2];
        float rasterThumbScale[2] = {0, 0};

        // Published by the raster thread, read by the compositor.
        mutable std::mutex shownMutex;
        Shown shown;
        uint64_t shownPasses = 0;  // the pass Shown is of
        bool published = false;
    };
    uint32_t pass_;
    std::unordered_map<uint32_t, std::unique_ptr<Layer>> layers_;
    std::atomic<uint64_t> rasters_{0};
};

}  // namespace bro::engine
