#pragma once

// The draw traversal's span cache. A document's paint is recorded per element
// box (DrawTraversal::drawElementContent: the box and its in-flow subtree)
// into a CommandBuffer of its own, which the buffer above it replays in place
// (Cmd_ReplaySpan). The next pass replays the same span again, without walking
// the subtree, when nothing it painted can have changed: the element's paint
// stamp (dom::Element::notePaintChanged — every mark, restyle, box change and
// scroll offset under it) is older than the span, and the span is drawn at
// the same place under the same clip. What a span cannot vouch for — a layer
// break, a form control, a video, an animated or still-decoding image, ... —
// makes it volatile: drawn, never reused (the reasons are counted).
//
// BRO_PAINT_CACHE=0 turns it off: every pass records everything, as before.

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
    Moved,          // drawn at another origin
    Clip,           // under another clip, or wrapped differently
    Context,        // the cache was emptied: viewport, fonts, top layer, ...
    // Volatile: recorded, never reused.
    LayerBreak,     // a canvas / WebGL / scene / iframe / remote view layer
    Control,        // <input>, <textarea>, <select>
    Video,
    Terminal,       // an inline <terminal>
    Svg,            // inline <svg> (its paint can read elements outside it)
    AnimatedImage,
    PendingImage,   // an image still decoding: the gap paints nothing
    ListOrdinal,    // a numbered list marker counts its siblings
    Count
};
const char* spanMissName(SpanMiss m);

struct PaintCacheStats {
    uint64_t reused = 0;
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
    bool sameOrigin(const SpanKey& o) const {
        return offX == o.offX && offY == o.offY && rootX == o.rootX && rootY == o.rootY;
    }
    bool sameClip(const SpanKey& o) const {
        return clipped == o.clipped && skipWrap == o.skipWrap &&
               (!clipped || (clipX == o.clipX && clipY == o.clipY && clipW == o.clipW && clipH == o.clipH));
    }
};

struct SpanEntry {
    uint32_t nodeId = 0;      // the element the key pointer named when recorded
    SpanKey key;
    uint64_t epoch = 0;       // dom::Element::paintEpoch() it was recorded in
    uint64_t lastDraw = 0;    // the cache's draw count when last recorded or reused
    SpanMiss volatileReason = SpanMiss::Count;  // Count: reusable
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
