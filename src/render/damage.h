#pragma once

// The damage between two recordings of one document: the region of the
// surface they paint whose pixels can differ. A raster pass that holds the
// older recording's pixels then redraws only that region, replaying the new
// recording clipped to it (Engine::replayAppLayers).
//
// The recordings are compared in lockstep (render/damage.cpp). Spans held by
// the same pointer under the same transform are the same pixels; two
// recordings of one element (CommandBuffer::ownerId) are compared inside;
// draws that differ add their bounds under the transform they are drawn with
// (a filter layer's whole bounds when drawn inside one); once the save /
// transform / clip structure of a level differs, the rest of that level is
// damaged in both. A changed layer break or surface break (the surfaces
// themselves would split differently), a clear, a backdrop filter anywhere
// (it samples pixels a rect cannot bound) or a perspective transform is
// damage everywhere. Bounds are in the recordings' own space, CSS px.

#include "render/command_buffer.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace bro::render {

struct Damage {
    bool full = false;    // everywhere
    bool empty = true;    // nowhere (when not full)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    void addRect(float ax0, float ay0, float ax1, float ay1) {
        if (full || !(ax1 > ax0 && ay1 > ay0)) return;
        if (empty) {
            x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1;
            empty = false;
            return;
        }
        x0 = std::min(x0, ax0); y0 = std::min(y0, ay0);
        x1 = std::max(x1, ax1); y1 = std::max(y1, ay1);
    }
    void setFull() { full = true; empty = false; }
    void unite(const Damage& o) {
        if (o.full) setFull();
        else if (!o.empty) addRect(o.x0, o.y0, o.x1, o.y1);
    }
};

// What changed from `older` to `newer`, two recordings of the same pass.
Damage diffDamage(const CommandBuffer& older, const CommandBuffer& newer);

// The damage each recording of a pass brought (from the one before it), by
// generation, for the last few: a surface that holds generation `held` needs
// the union over (held, now] redrawn. Surfaces are double-buffered, so one
// usually holds the recording before last.
class DamageHistory {
public:
    void push(uint64_t gen, const Damage& d) {
        entries_.push_back({gen, d});
        if (entries_.size() > kKeep) entries_.erase(entries_.begin());
    }
    void clear() { entries_.clear(); }
    // False when (held, gen] is not all here: redraw everything.
    bool since(uint64_t held, uint64_t gen, Damage& out) const {
        out = Damage{};
        if (held == 0 || held > gen) return false;
        for (uint64_t g = held + 1; g <= gen; ++g) {
            const Damage* d = find(g);
            if (!d) return false;
            out.unite(*d);
            if (out.full) return true;
        }
        return true;
    }

private:
    static constexpr size_t kKeep = 8;
    const Damage* find(uint64_t gen) const {
        for (const auto& e : entries_)
            if (e.first == gen) return &e.second;
        return nullptr;
    }
    std::vector<std::pair<uint64_t, Damage>> entries_;
};

}  // namespace bro::render
