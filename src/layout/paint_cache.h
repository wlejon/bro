#pragma once

// The draw traversal's span cache. A document's paint is recorded per element
// box (DrawTraversal::drawElementContent: the box and its in-flow subtree)
// into a CommandBuffer of its own, which the buffer above it replays in place
// (Cmd_ReplaySpan). The next pass replays the same span again, without walking
// the subtree, when nothing it painted can have changed: the element's paint
// stamp (dom::Element::notePaintChanged — every mark, restyle, box change and
// scroll offset under it) is older than the span, and the span is drawn at
// the same place under the same clip. What a span cannot vouch for — a layer
// break, a focused form control, a video, an animated or still-decoding
// image, ... — makes it volatile: drawn, never reused (the reasons are
// counted). Inline <svg> is cached (its paint is its own subtree's); a
// numbered list marker's span also checks its parent (SpanEntry::readsParent).
//
// A test can turn it off (setPaintCache in headless) to record a pass from
// scratch, the frame tests/layout/test_paint_cache_fuzz.js compares against.

#include "render/command_buffer.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace bro::dom { class Element; }

namespace bro::layout {

// Why a span was recorded rather than reused.
enum class SpanMiss : uint8_t {
    New,            // no span for this element yet (or it was pruned)
    Changed,        // something under it was stamped since it was recorded
    Moved,          // a pinned one (a viewport place, a control) drawn elsewhere
    Clip,           // its clip moved past what it culled, or wrapped differently
    Scrolled,       // a scroller, scrolled (a scroll stamps its parent, not it)
    Context,        // the cache was emptied: viewport, fonts, top layer, ...
    // Volatile: recorded, never reused.
    LayerBreak,     // a canvas / WebGL / scene / iframe / remote view layer
    Control,        // a focused <input> / <textarea> (caret, selection, scroll)
    Video,
    Terminal,       // an inline <terminal>
    AnimatedImage,
    PendingImage,   // an image still decoding: the gap paints nothing
    Count
};
const char* spanMissName(SpanMiss m);

struct PaintCacheStats {
    uint64_t reused = 0;
    uint64_t reusedScrolled = 0;  // of those, translated: a scroll moved them (document or a scroller)
    uint64_t recorded = 0;
    uint64_t misses[static_cast<size_t>(SpanMiss::Count)] = {};
};

// Where and how a span was drawn: the origin passed to drawElementContent,
// the pass's root offset, the clip it was culled against, and whether its
// stacking-context wrappers were applied around it.
struct SpanKey {
    float offX = 0, offY = 0, rootX = 0, rootY = 0;
    float clipX = 0, clipY = 0, clipW = 0, clipH = 0;
    bool clipped = false;
    bool skipWrap = false;
    // The element's own scroll offset: a scroll stamps the scroller's parent
    // and not the scroller (its content's spans, and a scroll layer's
    // recording, stand), so a span of a scroller painted in place is only
    // reused at the scroll it was recorded at.
    float scrollX = 0, scrollY = 0;
    // How far a box drawn at this key stands from where `rec` recorded it.
    float dx(const SpanKey& rec) const { return offX - rec.offX; }
    float dy(const SpanKey& rec) const { return offY - rec.offY; }
    bool sameOrigin(const SpanKey& rec) const { return near(offX, rec.offX) && near(offY, rec.offY); }
    // Whether paint recorded at `rec`, moved to this key's origin, is what
    // drawing here would record. A box paints relative to its origin (a
    // document scroll, an ancestor scroller's scroll, moves the origin and
    // nothing else), except for what the walk culled: boxes outside the clip
    // it was recorded under, by more than kCullSlack. So the clip here, in the
    // box's own terms, has to lie inside the recorded one grown by the slack:
    // then nothing culled then can show now.
    bool fitsIn(const SpanKey& rec) const {
        if (clipped != rec.clipped || skipWrap != rec.skipWrap) return false;
        if (!clipped) return true;
        const float x0 = clipX - offX, y0 = clipY - offY;
        const float rx0 = rec.clipX - rec.offX - kCullSlack, ry0 = rec.clipY - rec.offY - kCullSlack;
        return x0 >= rx0 - 0.01f && y0 >= ry0 - 0.01f &&
               x0 + clipW <= rx0 + rec.clipW + 2 * kCullSlack + 0.01f &&
               y0 + clipH <= ry0 + rec.clipH + 2 * kCullSlack + 0.01f;
    }
    static bool near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }
    // Boxes this far outside the clip a walk draws under are culled
    // (DrawTraversal::paintElement): their shadows and outlines spill less.
    static constexpr float kCullSlack = 256.0f;
};

struct SpanEntry {
    uint32_t nodeId = 0;      // the element the key pointer named when recorded
    SpanKey key;
    uint64_t epoch = 0;       // dom::Element::paintEpoch() it was recorded in
    uint64_t lastDraw = 0;    // the cache's draw count when last recorded or reused
    SpanMiss volatileReason = SpanMiss::Count;  // Count: reusable
    // It paints something at a place of the viewport (the html/body
    // background), not of the document: reused only at the root offset it
    // was recorded at, never translated for a scroll.
    bool pinned = false;
    // It paints something its parent element decides (a numbered marker's
    // ordinal): reused only while the parent's stamp is older than it too.
    bool readsParent = false;
    std::shared_ptr<render::CommandBuffer> buffer;
};

// What every span in one cache was recorded under: a change empties it.
struct PaintCacheContext {
    int viewportW = 0, viewportH = 0, viewportTop = 0;
    std::string basePath;
    bool terminalLayers = false;
    bool shellClientWindows = false;
    uint64_t fontGeneration = 0;
    uint64_t topLayerHash = 0;
    uint64_t frameHash = 0;
    bool operator==(const PaintCacheContext&) const = default;
};

// One document root's spans.
struct PaintCache {
    uint32_t rootNodeId = 0;
    PaintCacheContext context;
    uint64_t draws = 0;
    uint64_t lastPruneDraw = 0;
    std::unordered_map<const dom::Element*, SpanEntry> entries;
};

}  // namespace bro::layout
