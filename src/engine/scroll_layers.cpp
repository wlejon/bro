#include "engine/scroll_layers.h"

#include "dom/element.h"
#include "render/command_replayer.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

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
        // The window in device px at the renderer's scale, its corner at the
        // sub-pixel phase where the pass's scroll puts it: the compositor
        // draws the surface pixel for pixel at a whole device pixel, so what
        // it holds is rastered exactly where painting in place would put it.
        const float s = renderer->deviceScale();
        const float px = phase(l.contentX + l.winX - l.scrollX, s), py = phase(l.contentY + l.winY - l.scrollY, s);
        // A scroll by whole device pixels draws the same surface elsewhere.
        auto samePhase = [](float a, float b) { return std::abs(a - b) < 1.0f / 512.0f; };
        const bool draw = l.recorded || s != l.rasterScale || l.winX != l.rasterWinX ||
                          l.winY != l.rasterWinY || !samePhase(px, l.rasterPhaseX) || !samePhase(py, l.rasterPhaseY);
        l.recorded = false;
        if (!draw) {
            std::lock_guard<std::mutex> lock(l.shownMutex);
            l.shown.scrollX = l.scrollX;
            l.shown.scrollY = l.scrollY;
            continue;
        }
        const int w = std::max(1, static_cast<int>(std::ceil(l.winW * s + px)));
        const int h = std::max(1, static_cast<int>(std::ceil(l.winH * s + py)));
        if (l.surfW != w || l.surfH != h) {
            l.spare.reset();
            l.surfW = w;
            l.surfH = h;
        }
        renderer->fitLayerSurface(l.surface, w, h);
        if (!l.surface) {
            std::lock_guard<std::mutex> lock(l.shownMutex);
            l.shown = Shown{};
            l.published = false;
            continue;
        }
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

        Shown next;
        next.winX = l.winX;
        next.winY = l.winY;
        next.phaseX = px;
        next.phaseY = py;
        next.scrollX = l.scrollX;
        next.scrollY = l.scrollY;
        auto publish = [&l](Shown next) {
            std::lock_guard<std::mutex> lock(l.shownMutex);
            next.generation = l.shown.generation + 1;
            l.shown = std::move(next);
            l.published = true;
        };
        // As an iframe's: a CPU snapshot, or the GPU image once submitted
        // (the next raster draws into the other surface).
        if (l.surface.isGpu()) {
            next.gpu = l.surface.image;
            renderer->afterSubmit([publish, next = std::move(next)]() mutable { publish(std::move(next)); });
            std::swap(l.surface, l.spare);
        } else {
            next.image = l.surface.surface->makeImageSnapshot();
            publish(std::move(next));
        }
    }
}

bool ScrollLayerSet::shown(uint32_t elementId, Shown& out) const {
    auto it = layers_.find(elementId);
    if (it == layers_.end()) return false;
    const Layer& l = *it->second;
    std::lock_guard<std::mutex> lock(l.shownMutex);
    if (!l.published) return false;
    out = l.shown;
    return true;
}

}  // namespace bro::engine
