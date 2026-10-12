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

#include <include/core/SkRect.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace bro::render {

struct Damage {
    bool full = false;    // everywhere
    bool empty = true;    // nowhere (when not full)
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // the union
    // The same, as a few rects (at most kMaxRects, each new one merged into
    // the one it grows least once there are that many): a row taken out at
    // the top of a list and one put in at the bottom are two strips, not the
    // list. A scroll layer redraws these (engine/scroll_layers.h); the page's
    // surfaces the union.
    struct Rect { float x0, y0, x1, y1; };
    static constexpr size_t kMaxRects = 8;
    std::vector<Rect> rects;

    void addRect(float ax0, float ay0, float ax1, float ay1) {
        if (full || !(ax1 > ax0 && ay1 > ay0)) return;
        if (empty) {
            x0 = ax0; y0 = ay0; x1 = ax1; y1 = ay1;
            empty = false;
        } else {
            x0 = std::min(x0, ax0); y0 = std::min(y0, ay0);
            x1 = std::max(x1, ax1); y1 = std::max(y1, ay1);
        }
        Rect r{ax0, ay0, ax1, ay1};
        auto area = [](const Rect& a) { return (a.x1 - a.x0) * (a.y1 - a.y0); };
        auto merged = [](const Rect& a, const Rect& b) {
            return Rect{std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
        };
        // One it touches takes it.
        for (Rect& e : rects) {
            if (r.x0 <= e.x1 && e.x0 <= r.x1 && r.y0 <= e.y1 && e.y0 <= r.y1) {
                e = merged(e, r);
                return;
            }
        }
        if (rects.size() < kMaxRects) {
            rects.push_back(r);
            return;
        }
        size_t best = 0;
        float bestGrowth = 0;
        for (size_t k = 0; k < rects.size(); ++k) {
            const float g = area(merged(rects[k], r)) - area(rects[k]);
            if (k == 0 || g < bestGrowth) { best = k; bestGrowth = g; }
        }
        rects[best] = merged(rects[best], r);
    }
    void setFull() { full = true; empty = false; rects.clear(); }
    void unite(const Damage& o) {
        if (o.full) setFull();
        else for (const Rect& r : o.rects) addRect(r.x0, r.y0, r.x1, r.y1);
    }
};

// What changed from `older` to `newer`, two recordings of the same pass.
Damage diffDamage(const CommandBuffer& older, const CommandBuffer& newer);

// The bounds of what `buf` draws, in its own space (cached on it). False:
// unbounded, or not known.
bool commandBufferBounds(const CommandBuffer& buf, SkRect& out);

// `area` grown by every draw of `buf`'s own (a span whole) that reaches into
// it, in `buf`'s space: drawn clipped to that, nothing that touches `area`
// crosses the clip (Skia's CPU rasterizer antialiases a path the clip cuts
// otherwise than a whole one, inside the clip too). False: one is unbounded,
// or one reaching it is in a layer (whose device is the clip it opens under:
// what it holds rasters otherwise under another clip).
bool coveringBounds(const CommandBuffer& buf, const SkRect& area, SkRect& out);

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
