#include "engine/scroll_layers.h"

#include "dom/element.h"
#include "render/command_replayer.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

namespace {

// A scroll by whole device pixels draws the same surface elsewhere.
bool samePhase(float a, float b) { return std::abs(a - b) < 1.0f / 512.0f; }
bool wholePixels(float d) { return std::abs(d - std::round(d)) < 1.0f / 512.0f; }

}  // namespace

layout::ScrollLayerRecord ScrollLayerSet::beginScrollLayer(dom::Element* scroller,
                                                           const layout::ScrollLayerPlace& place, bool mayReuse) {
    if (!scroller || place.viewW <= 0.0f || place.viewH <= 0.0f) return {};
    auto& slot = layers_[scroller->nodeId()];
    if (!slot) slot = std::make_unique<Layer>();
    Layer& l = *slot;
    const float winW = place.viewW + 2 * kMargin, winH = place.viewH + 2 * kMargin;
    // The recording stands while nothing under the scroller was stamped
    // since it was made and its window still holds the view.
    const bool holds = place.viewX >= l.winX && place.viewY >= l.winY &&
                       place.viewX + place.viewW <= l.winX + l.winW && place.viewY + place.viewH <= l.winY + l.winH;
    const bool reuse = mayReuse && l.reusable && l.epoch != 0 && scroller->paintStamp() < l.epoch &&
                       l.winW == winW && l.winH == winH && holds;
    l.contentX = place.contentX;
    l.contentY = place.contentY;
    l.scrollX = place.scrollX;
    l.scrollY = place.scrollY;
    l.viewOffX = place.viewX - place.scrollX;
    l.viewOffY = place.viewY - place.scrollY;
    l.viewW = place.viewW;
    l.viewH = place.viewH;
    l.liveX.store(place.scrollX, std::memory_order_relaxed);
    l.liveY.store(place.scrollY, std::memory_order_relaxed);
    ++l.passes;
    // The pass's scrollbars say these again (setThumb, setHasNestedScroller).
    l.thumbs[0].on = l.thumbs[1].on = false;
    l.nestedScroller = false;
    l.seen = true;
    if (!reuse) {
        l.cmd.clear();
        l.winX = place.viewX - kMargin;
        l.winY = place.viewY - kMargin;
        l.winW = winW;
        l.winH = winH;
        l.epoch = dom::Element::paintEpoch();
        l.reusable = false;
        l.recorded = true;
    }
    return {&l.cmd, reuse, l.winX, l.winY, l.winW, l.winH};
}

void ScrollLayerSet::endScrollLayer(dom::Element* scroller, bool reusable) {
    if (!scroller) return;
    auto it = layers_.find(scroller->nodeId());
    if (it != layers_.end()) it->second->reusable = reusable;
}

void ScrollLayerSet::setThumb(uint32_t elementId, int axis, float w, float h, float crossPos, float scrollRange,
                              float thumbRange, bromath::Color color) {
    auto it = layers_.find(elementId);
    if (it == layers_.end() || axis < 0 || axis > 1) return;
    auto& t = it->second->thumbs[axis];
    t.on = true;
    t.w = w;
    t.h = h;
    t.crossPos = crossPos;
    t.scrollRange = scrollRange;
    t.thumbRange = thumbRange;
    t.color = color;
}

void ScrollLayerSet::setHasNestedScroller(uint32_t elementId) {
    auto it = layers_.find(elementId);
    if (it != layers_.end()) it->second->nestedScroller = true;
}

bool ScrollLayerSet::scrollTo(dom::Element* scroller, float scrollX, float scrollY, float scale) {
    if (!scroller) return false;
    auto it = layers_.find(scroller->nodeId());
    if (it == layers_.end()) return false;
    Layer& l = *it->second;
    if (!l.reusable || l.nestedScroller || l.epoch == 0 || scroller->paintStamp() >= l.epoch) return false;
    const float vx = scrollX + l.viewOffX, vy = scrollY + l.viewOffY;
    if (vx < l.winX || vy < l.winY || vx + l.viewW > l.winX + l.winW || vy + l.viewH > l.winY + l.winH) return false;
    if (!wholePixels((scrollX - l.scrollX) * scale) || !wholePixels((scrollY - l.scrollY) * scale)) return false;
    l.liveX.store(scrollX, std::memory_order_relaxed);
    l.liveY.store(scrollY, std::memory_order_relaxed);
    return true;
}

void ScrollLayerSet::beginPass() {
    for (auto& [id, l] : layers_) l->seen = false;
}

void ScrollLayerSet::endPass() {
    for (auto it = layers_.begin(); it != layers_.end();) {
        if (!it->second->seen) it = layers_.erase(it);
        else ++it;
    }
}

void ScrollLayerSet::replay(render::SkiaRenderer* renderer) {
    if (!renderer) return;
    for (auto& [id, lp] : layers_) {
        Layer& l = *lp;
        const float s = renderer->deviceScale();
        // What this pass publishes: the content's picture when it was drawn
        // again, each thumb's when it was, and always the pass's scroll.
        struct Update {
            bool content = false;
            Shown pic;
            bool thumbDrawn[2] = {false, false};
            ThumbShown thumb[2];
            float scrollX = 0, scrollY = 0;
            uint64_t passes = 0;
        } up;
        up.scrollX = l.scrollX;
        up.scrollY = l.scrollY;
        up.passes = l.passes;
        bool gpu = false;

        // The window in device px at the renderer's scale, its corner at the
        // sub-pixel phase where the pass's scroll puts it: the compositor
        // draws the surface pixel for pixel at a whole device pixel, so what
        // it holds is rastered exactly where painting in place would put it.
        const float px = phase(l.contentX + l.winX - l.scrollX, s), py = phase(l.contentY + l.winY - l.scrollY, s);
        const bool draw = l.recorded || s != l.rasterScale || l.winX != l.rasterWinX || l.winY != l.rasterWinY ||
                          !samePhase(px, l.rasterPhaseX) || !samePhase(py, l.rasterPhaseY);
        l.recorded = false;
        if (draw) {
            const int w = std::max(1, static_cast<int>(std::ceil(l.winW * s + px)));
            const int h = std::max(1, static_cast<int>(std::ceil(l.winH * s + py)));
            if (l.surfW != w || l.surfH != h) {
                l.spare.reset();
                l.surfW = w;
                l.surfH = h;
            }
            renderer->fitLayerSurface(l.surface, w, h);
            if (l.surface) {
                auto prev = renderer->switchSurface(l.surface.surface);
                renderer->translate(px / s - l.winX, py / s - l.winY);
                render::CommandReplayer replayer(renderer);
                replayer.setLayerBreakHandler([](const render::LayerSource&, const render::LayerQuad&) {});
                replayer.replay(l.cmd);
                renderer->switchSurface(prev);
                rasters_.fetch_add(1, std::memory_order_relaxed);
                l.rasterScale = s;
                l.rasterWinX = l.winX;
                l.rasterWinY = l.winY;
                l.rasterPhaseX = px;
                l.rasterPhaseY = py;
                up.content = true;
                up.pic.winX = l.winX;
                up.pic.winY = l.winY;
                up.pic.phaseX = px;
                up.pic.phaseY = py;
                // As an iframe's: a CPU snapshot, or the GPU image once
                // submitted (the next raster draws into the other surface).
                if (l.surface.isGpu()) {
                    up.pic.gpu = l.surface.image;
                    std::swap(l.surface, l.spare);
                    gpu = true;
                } else {
                    up.pic.image = l.surface.surface->makeImageSnapshot();
                }
            }
        }

        // Each thumb: its rect in its colour, at the phase its place across
        // the bar has (along it, it lands on a whole device pixel).
        for (int a = 0; a < 2; ++a) {
            const Layer::Thumb& t = l.thumbs[a];
            ThumbShown& ts = up.thumb[a];
            ts.scrollRange = t.scrollRange;
            ts.thumbRange = t.thumbRange;
            ts.phase = phase(t.crossPos, s);
            if (!t.on) {
                if (l.rasterThumb[a].on) up.thumbDrawn[a] = true;  // gone: cleared
                l.rasterThumb[a] = t;
                continue;
            }
            if (t == l.rasterThumb[a] && s == l.rasterThumbScale[a]) continue;
            const float w = a == 0 ? t.w * s + ts.phase : t.w * s, h = a == 0 ? t.h * s : t.h * s + ts.phase;
            render::LayerSurface& surf = l.thumbSurface[a];
            renderer->fitLayerSurface(surf, std::max(1, static_cast<int>(std::ceil(w))),
                                      std::max(1, static_cast<int>(std::ceil(h))));
            if (!surf) continue;
            auto prev = renderer->switchSurface(surf.surface);
            renderer->fillRect(a == 0 ? ts.phase / s : 0.0f, a == 0 ? 0.0f : ts.phase / s, t.w, t.h, t.color);
            renderer->switchSurface(prev);
            l.rasterThumb[a] = t;
            l.rasterThumbScale[a] = s;
            up.thumbDrawn[a] = true;
            if (surf.isGpu()) {
                ts.gpu = surf.image;
                std::swap(l.thumbSurface[a], l.thumbSpare[a]);
                gpu = true;
            } else {
                ts.image = surf.surface->makeImageSnapshot();
            }
        }

        auto publish = [&l](Update up) {
            std::lock_guard<std::mutex> lock(l.shownMutex);
            if (up.content) {
                const uint64_t generation = l.shown.generation + 1;
                ThumbShown thumbs[2] = {l.shown.thumbs[0], l.shown.thumbs[1]};
                l.shown = std::move(up.pic);
                l.shown.generation = generation;
                l.shown.thumbs[0] = thumbs[0];
                l.shown.thumbs[1] = thumbs[1];
                l.published = true;
            }
            for (int a = 0; a < 2; ++a) {
                ThumbShown& ts = l.shown.thumbs[a];
                ts.scrollRange = up.thumb[a].scrollRange;
                ts.thumbRange = up.thumb[a].thumbRange;
                ts.phase = up.thumb[a].phase;
                if (up.thumbDrawn[a]) {
                    ts.image = std::move(up.thumb[a].image);
                    ts.gpu = std::move(up.thumb[a].gpu);
                    ++l.shown.generation;
                }
            }
            l.shown.scrollX = up.scrollX;
            l.shown.scrollY = up.scrollY;
            l.shownPasses = up.passes;
        };
        if (gpu) renderer->afterSubmit([publish, up = std::move(up)]() mutable { publish(std::move(up)); });
        else publish(std::move(up));
    }
}

bool ScrollLayerSet::shown(uint32_t elementId, Shown& out) const {
    auto it = layers_.find(elementId);
    if (it == layers_.end()) return false;
    const Layer& l = *it->second;
    std::lock_guard<std::mutex> lock(l.shownMutex);
    if (!l.published) return false;
    out = l.shown;
    // The picture of the latest pass: where the main thread has scrolled it since.
    if (l.shownPasses == l.passes) {
        out.scrollX = l.liveX.load(std::memory_order_relaxed);
        out.scrollY = l.liveY.load(std::memory_order_relaxed);
    }
    return true;
}

}  // namespace bro::engine
