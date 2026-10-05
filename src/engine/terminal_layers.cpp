#include "engine/terminal_layers.h"

#include "engine/device_scale.h"
#include "layout/el_terminal.h"
#include "layout/term_layer.h"
#include "render/command_replayer.h"
#include "render/skia_backend.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace bro::engine {

bool terminalLayersEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("BRO_TERMINAL_LAYER");
        return !(v && v[0] == '0' && v[1] == '\0');
    }();
    return on;
}

TerminalLayers::TerminalLayers() = default;
TerminalLayers::~TerminalLayers() = default;

void TerminalLayers::record(float scale) {
    if (!terminalLayersEnabled()) return;
    std::unordered_map<uint64_t, bool> live;
    layout::ElTerminal::forEach([&](layout::ElTerminal& t) {
        std::shared_ptr<layout::TermLayer> layer = t.layer();
        if (!layer) return;
        live[layer->id] = true;
        auto& slot = entries_[layer->id];
        if (!slot) {
            slot = std::make_unique<Entry>();
            slot->layer = layer;
        }
        t.recordLayer(scale);
    });
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (live.count(it->first)) {
            ++it;
            continue;
        }
        // Its surfaces may be mid-use on the replaying thread's GPU work:
        // that thread frees them at its next replay.
        retired_.push_back(std::move(it->second));
        it = entries_.erase(it);
    }
}

void TerminalLayers::replay(render::SkiaRenderer* renderer) {
    if (!renderer) return;
    retired_.clear();
    for (auto& [id, e] : entries_) {
        layout::TermLayer& layer = *e->layer;
        if (layer.generation == e->replayedGeneration) continue;
        e->replayedGeneration = layer.generation;
        if (layer.boxW <= 0 || layer.boxH <= 0 || layer.commands.commandCount() == 0) {
            e->published.clear();
            continue;
        }
        // Device px at the scale it was recorded for (the renderer's current
        // one: the app pass's); the compositor quad stays in CSS px.
        DeviceScale ds;
        ds.render = renderer->deviceScale();
        const int bw = ds.toDevice(layer.boxW), bh = ds.toDevice(layer.boxH);
        if (e->surfW != bw || e->surfH != bh) {
            e->spare.reset();
            e->surfW = bw;
            e->surfH = bh;
        }
        renderer->fitLayerSurface(e->surface, bw, bh);
        if (!e->surface) {
            e->published.clear();
            continue;
        }
        auto prev = renderer->switchSurface(e->surface.surface);
        render::CommandReplayer replayer(renderer);
        replayer.replay(layer.commands);
        renderer->switchSurface(prev);
        ++replays_;
        // The compositor reads what is published, never the surface being
        // drawn: a CPU snapshot, or the GPU image once the frame is submitted.
        if (e->surface.isGpu()) {
            renderer->afterSubmit([&published = e->published, image = e->surface.image]() mutable {
                published.publish(std::move(image));
            });
            std::swap(e->surface, e->spare);
        } else {
            e->published.publish(e->surface.surface->makeImageSnapshot());
        }
    }
}

const PublishedFrame* TerminalLayers::published(uint64_t id) const {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second->published;
}

void TerminalLayers::releaseAll() {
    retired_.clear();
    for (auto& [id, e] : entries_) {
        e->surface.reset();
        e->spare.reset();
        e->surfW = e->surfH = 0;
        e->published.clear();
        e->replayedGeneration = 0;  // replayed afresh if the thread comes back
    }
}

} // namespace bro::engine
