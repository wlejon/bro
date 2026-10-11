#include "engine/scroll_layers.h"

#include "dom/element.h"
#include "render/command_replayer.h"

#include <algorithm>
#include <cmath>

namespace bro::engine {

render::CommandBuffer* ScrollLayerSet::beginScrollLayer(dom::Element* scroller, float winX, float winY, float winW,
                                                        float winH, float originX, float originY) {
    if (!scroller || winW <= 0.0f || winH <= 0.0f) return nullptr;
    auto& slot = layers_[scroller->nodeId()];
    if (!slot) slot = std::make_unique<Layer>();
    Layer& l = *slot;
    l.cmd.clear();
    l.winX = winX;
    l.winY = winY;
    l.winW = winW;
    l.winH = winH;
    l.originX = originX;
    l.originY = originY;
    l.seen = true;
    l.needsReplay = true;
    return &l.cmd;
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
        if (!l.needsReplay && l.published.generation() > 0) continue;
        l.needsReplay = false;
        // The window in device px at the renderer's scale, its corner at the
        // quad origin's sub-pixel phase: the compositor draws the surface
        // pixel for pixel at a whole device pixel, so what it holds is
        // rastered exactly where painting in place would put it.
        const float s = renderer->deviceScale();
        const float px = phase(l.originX, s), py = phase(l.originY, s);
        const int w = std::max(1, static_cast<int>(std::ceil(l.winW * s + px)));
        const int h = std::max(1, static_cast<int>(std::ceil(l.winH * s + py)));
        if (l.surfW != w || l.surfH != h) {
            l.spare.reset();
            l.surfW = w;
            l.surfH = h;
        }
        renderer->fitLayerSurface(l.surface, w, h);
        if (!l.surface) {
            l.published.clear();
            continue;
        }
        auto prev = renderer->switchSurface(l.surface.surface);
        renderer->translate(px / s - l.winX, py / s - l.winY);
        render::CommandReplayer replayer(renderer);
        replayer.setLayerBreakHandler([](const render::LayerSource&, const render::LayerQuad&) {});
        replayer.replay(l.cmd);
        renderer->switchSurface(prev);
        // As an iframe's: a CPU snapshot, or the GPU image once submitted
        // (the next replay draws into the other surface).
        if (l.surface.isGpu()) {
            renderer->afterSubmit([&published = l.published, image = l.surface.image]() mutable {
                published.publish(std::move(image));
            });
            std::swap(l.surface, l.spare);
        } else {
            l.published.publish(l.surface.surface->makeImageSnapshot());
        }
    }
}

const PublishedFrame* ScrollLayerSet::published(uint32_t elementId) const {
    auto it = layers_.find(elementId);
    return it == layers_.end() ? nullptr : &it->second->published;
}

}  // namespace bro::engine
