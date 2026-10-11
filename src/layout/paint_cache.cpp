// The span cache around DrawTraversal::paintElement (layout/paint_cache.h).

#include "layout/draw_traversal.h"
#include "layout/paint_cache.h"
#include "dom/document.h"
#include "dom/element.h"
#include "render/font_generation.h"
#include "render/recording_renderer.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace bro::layout {

const char* spanMissName(SpanMiss m) {
    switch (m) {
        case SpanMiss::New:           return "new";
        case SpanMiss::Changed:       return "changed";
        case SpanMiss::Moved:         return "moved";
        case SpanMiss::Clip:          return "clip";
        case SpanMiss::Context:       return "context";
        case SpanMiss::LayerBreak:    return "layerBreak";
        case SpanMiss::Control:       return "control";
        case SpanMiss::Video:         return "video";
        case SpanMiss::Terminal:      return "terminal";
        case SpanMiss::Svg:           return "svg";
        case SpanMiss::AnimatedImage: return "animatedImage";
        case SpanMiss::PendingImage:  return "pendingImage";
        case SpanMiss::ListOrdinal:   return "listOrdinal";
        case SpanMiss::Count:         break;
    }
    return "?";
}

namespace {

// -1: BRO_PAINT_CACHE decides; 0/1: setPaintCacheEnabled.
std::atomic<int> g_cacheOverride{-1};

bool envEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("BRO_PAINT_CACHE");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

// An element whose paint holds another element's: a span of its own is
// worth keeping. A leaf paints inline into the span around it.
bool isSpanCandidate(const dom::Element* elem) {
    if (elem->hasShadow()) return true;
    for (const dom::Node* n : elem->childNodes())
        if (n->nodeType() == dom::NodeType::Element) return true;
    return false;
}

uint64_t mixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// Spans kept unused this many passes are dropped (a span reused whole keeps
// its inner spans untouched, so this is also how long an unchanged subtree's
// inner spans survive it).
constexpr uint64_t kPruneAge = 120;

}  // namespace

bool DrawTraversal::paintCacheEnabled() {
    int o = g_cacheOverride.load(std::memory_order_relaxed);
    return o < 0 ? envEnabled() : o != 0;
}

void DrawTraversal::setPaintCacheEnabled(bool on) {
    g_cacheOverride.store(on ? 1 : 0, std::memory_order_relaxed);
}

PaintCache* DrawTraversal::beginPaintCache(dom::Element* root) {
    contextFlushed_ = false;
    if (!recorder_ || !recorder_->buffer() || !root || !paintCacheEnabled()) return nullptr;

    PaintCacheContext ctx;
    ctx.viewportW = viewportW_;
    ctx.viewportH = viewportH_;
    ctx.viewportTop = viewportTop_;
    ctx.basePath = basePath_;
    ctx.terminalLayers = terminalLayers_;
    ctx.shellClientWindows = shellClientWindows_;
    ctx.fontGeneration = render::fontGeneration();
    if (dom::Document* doc = root->document(); doc && doc->documentElement() == root)
        for (const auto& e : doc->topLayer())
            ctx.topLayerHash = mixHash(ctx.topLayerHash, reinterpret_cast<uintptr_t>(e.element));
    if (shellClientWindows_ && clientSlots_)
        for (const auto& s : *clientSlots_)
            ctx.frameHash = mixHash(ctx.frameHash, reinterpret_cast<uintptr_t>(s.frame));

    auto& slot = caches_[static_cast<int>(promotedElements_ ? paintMode_ : PaintMode::All)][root];
    if (!slot || slot->rootNodeId != root->nodeId()) {
        slot = std::make_unique<PaintCache>();
        slot->rootNodeId = root->nodeId();
        slot->context = ctx;
    } else if (!(slot->context == ctx)) {
        slot->entries.clear();
        slot->context = ctx;
        contextFlushed_ = true;
    }
    ++slot->draws;
    return slot.get();
}

void DrawTraversal::prunePaintCache(PaintCache& cache) {
    if (cache.draws - cache.lastPruneDraw < kPruneAge) return;
    cache.lastPruneDraw = cache.draws;
    for (auto it = cache.entries.begin(); it != cache.entries.end();) {
        if (cache.draws - it->second.lastDraw > kPruneAge) it = cache.entries.erase(it);
        else ++it;
    }
}

void DrawTraversal::drawElementContent(dom::Element* elem, float offsetX, float offsetY) {
    if (!activeCache_ || !elem || !isSpanCandidate(elem)) {
        paintElement(elem, offsetX, offsetY);
        return;
    }
    PaintCache& cache = *activeCache_;

    SpanKey key;
    key.offX = offsetX;
    key.offY = offsetY;
    key.rootX = rootOffsetX_;
    key.rootY = rootOffsetY_;
    key.clipped = currentClipRect(key.clipX, key.clipY, key.clipW, key.clipH);
    key.skipWrap = scRootSkipWrap_.count(elem) != 0;

    SpanMiss miss = contextFlushed_ ? SpanMiss::Context : SpanMiss::New;
    auto it = cache.entries.find(elem);
    SpanEntry* entry = nullptr;
    if (it != cache.entries.end() && it->second.nodeId == elem->nodeId()) {
        entry = &it->second;
        if (entry->volatileReason != SpanMiss::Count) miss = entry->volatileReason;
        else if (elem->paintStamp() >= entry->epoch) miss = SpanMiss::Changed;
        else if (!entry->key.sameOrigin(key)) miss = SpanMiss::Moved;
        else if (!entry->key.sameClip(key)) miss = SpanMiss::Clip;
        else {
            entry->lastDraw = cache.draws;
            recorder_->recordSpan(entry->buffer);
            ++cacheStats_.reused;
            return;
        }
    }
    ++cacheStats_.recorded;
    ++cacheStats_.misses[static_cast<size_t>(miss)];

    // Into a buffer of its own: the old one's when nothing else holds it
    // (no recording above it still replays it), else a fresh one.
    std::shared_ptr<render::CommandBuffer> buf;
    if (entry && entry->buffer && entry->buffer.use_count() == 1) {
        buf = std::move(entry->buffer);
        buf->clear();
    } else {
        buf = std::make_shared<render::CommandBuffer>();
    }
    render::CommandBuffer* outer = recorder_->buffer();
    const SpanMiss outerVolatile = std::exchange(spanVolatile_, SpanMiss::Count);
    recorder_->setBuffer(buf.get());
    paintElement(elem, offsetX, offsetY);
    recorder_->setBuffer(outer);
    const SpanMiss ownVolatile = spanVolatile_;
    spanVolatile_ = outerVolatile != SpanMiss::Count ? outerVolatile : ownVolatile;
    recorder_->recordSpan(buf);

    if (!entry) entry = &cache.entries[elem];
    entry->nodeId = elem->nodeId();
    entry->key = key;
    entry->epoch = dom::Element::paintEpoch();
    entry->lastDraw = cache.draws;
    entry->volatileReason = ownVolatile;
    entry->buffer = std::move(buf);
}

}  // namespace bro::layout
