// Scroll-layer promotion, piece splitting, and shell client-window emission for
// DrawTraversal (split from draw_traversal.cpp).

#include "layout/draw_traversal_internal.h"
#include "dom/element_geometry.h"
#include "dom/element_scroll.h"
#include "render/recording_renderer.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace bro::layout {

const char* scrollLayerVetoName(ScrollLayerVeto v) {
    switch (v) {
        case ScrollLayerVeto::None:         return "none";
        case ScrollLayerVeto::Nested:       return "nested";
        case ScrollLayerVeto::RoundedClip:  return "roundedClip";
        case ScrollLayerVeto::Effects:      return "effects";
        case ScrollLayerVeto::Positioned:   return "positioned";
        case ScrollLayerVeto::Sticky:       return "sticky";
        case ScrollLayerVeto::LayerContent: return "layerContent";
        case ScrollLayerVeto::Control:      return "control";
        case ScrollLayerVeto::Video:        return "video";
        case ScrollLayerVeto::Count:        break;
    }
    return "?";
}

namespace {

// An opacity, transform, filter, blend, clip-path or perspective: what the
// compositor, drawing a scroll layer's picture under a rectangular clip,
// would not apply to it.
bool hasPaintEffect(const htmlayout::css::ComputedStyle& s) {
    auto set = [&](const char* name, const char* none) {
        auto it = s.find(name);
        return it != s.end() && !it->second.empty() && it->second != none;
    };
    auto opIt = s.find("opacity");
    if (opIt != s.end() && !opIt->second.empty() && std::strtof(opIt->second.c_str(), nullptr) < 1.0f) return true;
    return set("transform", "none") || set("filter", "none") || set("backdrop-filter", "none") ||
           set("mix-blend-mode", "normal") || set("clip-path", "none") || set("perspective", "none") ||
           set("mask", "none") || set("mask-image", "none");
}

// Whether `e` is `root` or under it.
bool withinElement(const dom::Element* e, const dom::Element* root) {
    for (; e; e = e->parentElement())
        if (e == root) return true;
    return false;
}

// What in `root`'s content keeps it from being a scroll layer: a positioned
// box its scroll does not move (fixed, sticky, absolute against a block
// outside it) or one painted before it (a negative z-index), one that is
// composited on its own, a control, a video. Positioned boxes and stacking
// contexts it does move are pieces of its layer (ScrollLayerHost::
// beginScrollPiece).
ScrollLayerVeto scanScrollLayerContent(dom::Element* root) {
    ScrollLayerVeto found = ScrollLayerVeto::None;
    std::function<void(dom::Element*)> visit = [&](dom::Element* e) {
        if (found != ScrollLayerVeto::None) return;
        const auto& s = e->computedStyle();
        auto disp = s.find("display");
        if (disp != s.end() && disp->second == "none") return;
        auto pos = s.find("position");
        const std::string_view position = pos != s.end() ? std::string_view(pos->second) : std::string_view();
        if (position == "sticky") { found = ScrollLayerVeto::Sticky; return; }
        if (position == "fixed") {
            const dom::Element* cb = fixedContainingBlock(e);
            if (!cb || !withinElement(cb, root)) { found = ScrollLayerVeto::Positioned; return; }
        } else if (position == "absolute") {
            const dom::Element* cb = absoluteContainingBlock(e);
            if (!cb || !withinElement(cb, root)) { found = ScrollLayerVeto::Positioned; return; }
        }
        if (createsStackingContext(e, false)) {
            int z = 0;
            if (!getZIndex(s, z) && z < 0) { found = ScrollLayerVeto::Positioned; return; }
        }
        if (e->canvasScene() || e->webglContext() || e->kernelCanvas() || e->sceneGraph() || e->iframeDoc() ||
            e->terminalControl() || e->remoteViewControl()) {
            found = ScrollLayerVeto::LayerContent;
            return;
        }
        if (e->inputControl() || e->textareaControl() || e->selectControl()) { found = ScrollLayerVeto::Control; return; }
        if (e->videoControl()) { found = ScrollLayerVeto::Video; return; }
        e->forEachComposedChild([&](dom::Element* c) { visit(c); });
    };
    root->forEachComposedChild([&](dom::Element* c) { visit(c); });
    return found;
}

}  // namespace

ScrollLayerVeto DrawTraversal::scrollLayerVeto(dom::Element* elem, const htmlayout::css::ComputedStyle& style,
                                               float bw, float bh) {
    if (inScrollLayer_) return ScrollLayerVeto::Nested;
    if (!getRadii(style, bw, bh).isZero()) return ScrollLayerVeto::RoundedClip;
    if (isCollapsedTable(elem)) return ScrollLayerVeto::Effects;
    for (dom::Element* e = elem; e; e = e->layoutParent()) {
        const auto& s = e->computedStyle();
        if (hasPaintEffect(s)) return ScrollLayerVeto::Effects;
        if (e != elem && (overflowAxisClips(getOverflowX(s)) || overflowAxisClips(getOverflowY(s))) &&
            !getRadii(s, e->layoutBox().fullWidth(), e->layoutBox().fullHeight()).isZero())
            return ScrollLayerVeto::RoundedClip;
    }
    if (parentPerspective(elem) > 0) return ScrollLayerVeto::Effects;
    if (elem->scrollLayerScanEpoch() != 0 && elem->paintStamp() < elem->scrollLayerScanEpoch())
        return static_cast<ScrollLayerVeto>(elem->scrollLayerScanVeto());
    const ScrollLayerVeto v = scanScrollLayerContent(elem);
    elem->setScrollLayerScan(dom::Element::paintEpoch(), static_cast<uint8_t>(v));
    return v;
}

bool DrawTraversal::paintScrollLayer(dom::Element* elem, const htmlayout::css::ComputedStyle& style,
                                     const htmlayout::layout::LayoutBox& box, bool visible, float x, float y,
                                     float bw, float bh) {
    if (!scrollLayerHost_ || !recorder_ || !recorder_->buffer() || !layerBreakCb_) return false;
    auto scrolls = [](const std::string& ov) { return ov == "auto" || ov == "scroll"; };
    const bool scrollY = scrolls(getOverflowY(style)) && dom::maxScrollTopOf(elem) > 0.0f;
    const bool scrollX = scrolls(getOverflowX(style)) && dom::maxScrollLeftOf(elem) > 0.0f;
    if (!scrollX && !scrollY) return false;
    const ScrollLayerVeto veto = scrollLayerVeto(elem, style, bw, bh);
    if (veto != ScrollLayerVeto::None) {
        ++scrollLayerStats_.vetoes[static_cast<size_t>(veto)];
        return false;
    }

    ScrollLayerPlace place;
    place.contentX = x;
    place.contentY = y;
    place.scrollX = dom::clampedScrollLeftOf(elem);
    place.scrollY = dom::clampedScrollTopOf(elem);
    place.viewX = place.scrollX - box.padding.left;
    place.viewY = place.scrollY - box.padding.top;
    place.viewW = box.contentRect.width + box.padding.left + box.padding.right;
    place.viewH = box.contentRect.height + box.padding.top + box.padding.bottom;
    place.snapScale = snapScale_;
    place.scrollsX = scrollX;
    place.scrollsY = scrollY;
    if (!currentClipRect(place.clipX, place.clipY, place.clipW, place.clipH)) place.clipW = place.clipH = -1.0f;
    const ScrollLayerRecord rec = scrollLayerHost_->beginScrollLayer(elem, place, !contextFlushed_);
    if (!rec.buffer) return false;
    LayerBreak lb;
    lb.element = elem;
    lb.source = render::ScrollLayerSource{elem->nodeId(), scrollLayerHost_->scrollLayerPass()};
    lb.quad.x = x;
    lb.quad.y = y;
    lb.quad.w = place.viewW;
    lb.quad.h = place.viewH;
    if (!currentClipRect(lb.quad.clipX, lb.quad.clipY, lb.quad.clipW, lb.quad.clipH))
        lb.quad.clipW = lb.quad.clipH = -1.0f;
    noteVolatile(SpanMiss::LayerBreak);
    layerBreakCb_(lb);
    ++scrollLayerStats_.layers;
    PassScrollLayer& pl = passScrollLayers_[elem];
    pl = PassScrollLayer{};
    pl.quad = lb.quad;
    pl.scrollX = place.scrollX;
    pl.scrollY = place.scrollY;
    pl.winX = rec.winX;
    pl.winY = rec.winY;
    pl.winW = rec.winW;
    pl.winH = rec.winH;
    pl.reused = rec.reuse;
    if (rec.reuse) {
        ++scrollLayerStats_.reused;
        return true;
    }

    render::CommandBuffer* outer = recorder_->buffer();
    std::vector<ClipBox> clips = std::move(clipRectStack_);
    clipRectStack_.clear();
    pushClipRect(rec.winX, rec.winY, rec.winW, rec.winH);
    const bool wasIn = std::exchange(inScrollLayer_, true);
    const SpanMiss outerVolatile = std::exchange(spanVolatile_, SpanMiss::Count);
    const float outerSnapX = std::exchange(snapOriginX_, x - place.scrollX);
    const float outerSnapY = std::exchange(snapOriginY_, y - place.scrollY);
    recorder_->setBuffer(rec.buffer);
    paintContent(elem, style, box, visible, 0.0f, 0.0f);
    recorder_->setBuffer(outer);
    snapOriginX_ = outerSnapX;
    snapOriginY_ = outerSnapY;
    scrollLayerHost_->endScrollLayer(elem, spanVolatile_ == SpanMiss::Count);
    spanVolatile_ = outerVolatile;
    inScrollLayer_ = wasIn;
    clipRectStack_ = std::move(clips);
    return true;
}

dom::Element* DrawTraversal::pieceScrollerOf(dom::Element* elem) const {
    if (passScrollLayers_.empty() || inScrollLayer_ || !elem) return nullptr;
    for (dom::Element* p = elem->layoutParent(); p; p = p->layoutParent())
        if (passScrollLayers_.count(p)) return p;
    return nullptr;
}

bool DrawTraversal::clipInPiece(const ClipRect& c) const {
    if (!pieceScroller_) return true;
    return c.owner && c.owner != pieceScroller_ && withinElement(c.owner, pieceScroller_);
}

void DrawTraversal::paintWalkItems(std::vector<WalkItem>& items) {
    for (size_t i = 0; i < items.size();) {
        dom::Element* scroller = pieceScrollerOf(items[i].elem);
        size_t j = i + 1;
        if (!scroller) {
            items[i].paint();
            i = j;
            continue;
        }
        while (j < items.size() && pieceScrollerOf(items[j].elem) == scroller) ++j;
        paintScrollPiece(scroller, items, i, j);
        i = j;
    }
}

void DrawTraversal::paintScrollPiece(dom::Element* scroller, std::vector<WalkItem>& items, size_t from, size_t to) {
    PassScrollLayer& pl = passScrollLayers_[scroller];
    std::vector<uint32_t> entries;
    entries.reserve(to - from);
    for (size_t k = from; k < to; ++k) entries.push_back(items[k].elem->nodeId());
    const uint32_t index = pl.pieces;
    const ScrollLayerRecord rec = scrollLayerHost_->beginScrollPiece(scroller, index, entries, pl.reused);
    if (!rec.buffer) {
        for (size_t k = from; k < to; ++k) items[k].paint();
        return;
    }
    ++pl.pieces;
    LayerBreak lb;
    lb.element = scroller;
    lb.source = render::ScrollLayerSource{scroller->nodeId(), scrollLayerHost_->scrollLayerPass(),
                                          render::ScrollLayerSource::Piece, static_cast<uint16_t>(index)};
    lb.quad = pl.quad;
    noteVolatile(SpanMiss::LayerBreak);
    layerBreakCb_(lb);
    ++scrollLayerStats_.pieces;
    if (rec.reuse) {
        ++scrollLayerStats_.piecesReused;
        return;
    }

    const float dx = pl.scrollX - pl.quad.x, dy = pl.scrollY - pl.quad.y;
    render::CommandBuffer* outer = recorder_->buffer();
    std::vector<ClipBox> clips = std::move(clipRectStack_);
    clipRectStack_.clear();
    pushClipRect(rec.winX - dx, rec.winY - dy, rec.winW, rec.winH);
    const bool wasIn = std::exchange(inScrollLayer_, true);
    const dom::Element* wasPiece = std::exchange(pieceScroller_, scroller);
    const SpanMiss outerVolatile = std::exchange(spanVolatile_, SpanMiss::Count);
    recorder_->setBuffer(rec.buffer);
    renderer_->save();
    renderer_->translate(dx, dy);
    for (size_t k = from; k < to; ++k) items[k].paint();
    renderer_->restore();
    recorder_->setBuffer(outer);
    scrollLayerHost_->endScrollPiece(scroller, index, spanVolatile_ == SpanMiss::Count);
    spanVolatile_ = outerVolatile;
    pieceScroller_ = wasPiece;
    inScrollLayer_ = wasIn;
    clipRectStack_ = std::move(clips);
}

void DrawTraversal::emitClientWindows() {
    if (!layerBreakCb_) return;
    std::vector<render::ClientWindowRef> scratch;
    std::vector<render::ClientWindowRef>& refs = clientRefs_ ? *clientRefs_ : scratch;
    size_t first = refs.size();
    bool firstRun = true;
    auto flush = [&](uint32_t parts) {
        LayerBreak lb;
        render::ClientWindowsLayerSource src;
        src.list = clientListId_;
        src.first = static_cast<uint32_t>(first);
        src.count = static_cast<uint32_t>(refs.size() - first);
        src.parts = parts | (firstRun ? render::kClientLayersBelow : 0u);
        lb.source = src;
        lb.quad.w = static_cast<float>(viewportW_);
        lb.quad.h = static_cast<float>(viewportH_);
        layerBreakCb_(lb);
        first = refs.size();
        firstRun = false;
    };
    if (clientSlots_) {
        for (const auto& slot : *clientSlots_) {
            auto it = slot.frame ? frameSCs_.find(slot.frame) : frameSCs_.end();
            if (it == frameSCs_.end() || !it->second) {
                refs.push_back(render::ClientWindowRef{slot.windowId, false, 0.0f, 0.0f});
                continue;
            }
            if (refs.size() > first || firstRun) flush(0);
            StackingContext* sc = it->second.get();
            paintStackingContext(sc, false);
            const auto& box = slot.frame->layoutBox();
            const float bx = box.contentRect.x + sc->offsetX - box.padding.left - box.border.left;
            const float by = box.contentRect.y + sc->offsetY - box.padding.top - box.border.top;
            const float bw = box.contentRect.width + box.padding.left + box.padding.right + box.border.left +
                             box.border.right;
            const float bh = box.contentRect.height + box.padding.top + box.padding.bottom + box.border.top +
                             box.border.bottom;
            refs.push_back(render::ClientWindowRef{slot.windowId, true, bx + slot.insetLeft, by + slot.insetTop,
                                                   std::max(0.0f, bw - slot.insetLeft - slot.insetRight),
                                                   std::max(0.0f, bh - slot.insetTop - slot.insetBottom)});
            auto ov = frameOverlaySCs_.find(slot.frame);
            if (ov != frameOverlaySCs_.end() && !ov->second.empty()) {
                flush(0);
                for (auto& osc : ov->second)
                    if (osc) paintStackingContext(osc.get(), false);
            }
        }
    }
    flush(render::kClientLayersAbove);
}

}  // namespace bro::layout
