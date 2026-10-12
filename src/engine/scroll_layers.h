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
    // The same on an even grid, [0, 2): on the GPU a surface sits an even
    // number of device pixels from where painting in place puts its content.
    // Ganesh antialiases a rounded corner (FillRRectOp) and more with
    // hardware derivatives, taken over 2x2 pixel quads: at an odd offset its
    // pixels come out a few levels apart from the page's.
    static float evenPhase(float originCss, float scale) {
        const float d = originCss * scale;
        return d - 2.0f * std::floor(d * 0.5f);
    }
    // How far round the view a recorded window reaches (CSS px).
    static constexpr float kMargin = 256.0f;
    // An edge of a clip that does not cut (an axis the layer scrolls).
    static constexpr float kOpen = 1e30f;

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
    // pixels at `scale`. False: the page has to be recorded for it. On the
    // GPU a move by an odd number of device pixels leaves its surfaces off
    // the even grid they were rastered on (evenPhase): placed right, but
    // antialiased on the other parity; `rasterDue` then asks for a raster
    // pass (no record), which rasters it at the phase of its live scroll.
    bool scrollTo(dom::Element* scroller, float scrollX, float scrollY, float scale, bool* rasterDue = nullptr);

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
    // Layer surfaces rastered whole (a part each), and from the last one's
    // pixels with only what moved in or changed drawn (rasterPart), since startup.
    uint64_t rasters() const { return rasters_.load(std::memory_order_relaxed); }
    uint64_t partialRasters() const { return partialRasters_.load(std::memory_order_relaxed); }
    // A check for tests (headless setScrollLayerVerify): after each partial
    // raster, the same recording rastered whole beside it and compared pixel
    // for pixel; the pixels that differ, since startup. Partial must be whole.
    static void setVerifyPartial(bool on);
    static bool verifyPartial();
    uint64_t partialMismatch() const { return partialMismatch_.load(std::memory_order_relaxed); }

private:
    // One recording of a layer and the surfaces it is rastered into.
    struct Part {
        // Recorded by the main thread, read by the raster thread's replay.
        render::CommandBuffer cmd;  // content space
        // The recording before it, when that was the one last rastered
        // (prevValid): what changed between them is what a raster draws.
        render::CommandBuffer prevCmd;
        bool prevValid = false;
        std::vector<uint32_t> entries;  // a piece's: the elements whose paint it holds
        bool reusable = false;
        bool recorded = true;  // recorded since it was last rastered
        // The raster thread's: its surfaces (on the GPU drawn in turn, the
        // last raster in the spare once swapped; on the CPU one, the last
        // raster in it), where the CPU one holds its last raster (window,
        // phase, scale; valid false: nothing to keep), and the scratch a
        // partial raster draws in.
        struct Holds {
            bool valid = false;
            float winX = 0, winY = 0, phaseX = 0, phaseY = 0, scale = 0;
        };
        render::LayerSurface surface, spare, scratch;
        Holds holds;
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
        // Where content space landed (content box less scroll) and the snap
        // scale when its content was recorded (ScrollLayerPlace::snapScale).
        float recordOriginX = 0, recordOriginY = 0, recordSnapScale = 0;
        // The compositor's clip on the axes it does not scroll, in content
        // space (open on an axis that scrolls): the content part is rastered
        // under it, as it is drawn under it in place (ScrollLayerPlace::clipX).
        float fixX0 = -kOpen, fixY0 = -kOpen, fixX1 = kOpen, fixY1 = kOpen;
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
        // The live scroll its parts were last rastered at (the raster
        // thread's, read by scrollTo).
        std::atomic<float> rasterScrollX{0}, rasterScrollY{0};

        // The raster thread's: what it last drew the parts at, and the thumbs.
        float rasterWinX = 0, rasterWinY = 0, rasterPhaseX = -1, rasterPhaseY = -1, rasterScale = 0;
        float rasterFix[4] = {0, 0, 0, 0};
        render::LayerSurface thumbSurface[2], thumbSpare[2];
        Thumb rasterThumb[2];
        float rasterThumbScale[2] = {0, 0};

        // Published by the raster thread, read by the compositor.
        mutable std::mutex shownMutex;
        Shown shown;
        uint64_t shownPasses = 0;  // the pass Shown is of
        bool published = false;
    };
    // A CPU part's raster in place: its surface holds the last raster where
    // this one goes, and only what changed in its recording since is drawn
    // again. False: draw it whole (too much changed, or what changed is not
    // known). On the GPU a layer is rastered whole: a raster there costs a
    // recording of its own per surface and a put-back, about what the whole
    // layer does (scroll_layers.cpp, ScrollLayerSet::replay).
    bool rasterPart(render::SkiaRenderer* renderer, const Layer& l, Part& p, float px, float py, float s, int w,
                    int h);
    uint32_t pass_;
    std::unordered_map<uint32_t, std::unique_ptr<Layer>> layers_;
    std::atomic<uint64_t> rasters_{0};
    std::atomic<uint64_t> partialRasters_{0};
    std::atomic<uint64_t> partialMismatch_{0};
    // Its surfaces are the GPU's, so placed on even device pixels (evenPhase):
    // set by the raster thread, read by scrollTo (rasterDue).
    std::atomic<bool> evenGrid_{false};
};

}  // namespace bro::engine
