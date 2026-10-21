#pragma once
// What each shadow caster did since the last frame, for the cached shadow
// pages (shadow_pages.h): a caster that does not move is in the pages'
// cached depth, and its change invalidates the pages its bounds touch; one
// that moves is drawn every frame over that depth instead.
//
// A caster moves when it is told to (skinned, a vertex chunk, swaying
// vegetation: alwaysMoving) or when its change generation moved last frame;
// it holds still again after kSettleFrames frames without a change. Going
// from still to moving takes it out of the cached depth (its old bounds'
// pages are redrawn without it), coming to rest puts it back (its bounds'
// pages are redrawn with it), and a still caster that appears or goes
// redraws the pages it touches.

#include "scene/world/shadow_pages.h"

#include <bromath/aabb.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace bro::scene {

class ShadowCasters {
public:
    static constexpr int kSettleFrames = 8;

    void begin() { ++frame_; }
    /// Caster `id` this frame: its change generation and world bounds.
    /// True when it moves (drawn into the frame's overlays). One that holds
    /// still but `sways` (vegetation in a wind) is out of the cached depth of
    /// the sway level and finer and in the sway level's overlays, so a turn
    /// of the wind to or from a calm redraws the pages it touches.
    bool caster(uint32_t id, uint64_t generation, const bromath::AABB3& bounds, bool alwaysMoving, ShadowPages& pages,
                bool sways = false) {
        auto [it, added] = states_.try_emplace(id);
        State& s = it->second;
        if (added) {
            s.moving = alwaysMoving;
            if (!alwaysMoving) pages.invalidate(bounds);
        } else if (alwaysMoving || generation != s.generation) {
            if (!s.moving) pages.invalidate(s.bounds);
            s.moving = true;
            s.still = 0;
        } else if (s.moving && ++s.still >= kSettleFrames) {
            s.moving = false;
            pages.invalidate(bounds);
        }
        if (!added && !s.moving && sways != s.sways) pages.invalidate(bounds);
        s.generation = generation;
        s.bounds = bounds;
        s.sways = sways;
        s.frame = frame_;
        if (s.moving) pages.addDynamic(bounds);
        else if (sways) pages.addDynamic(bounds, true);
        return s.moving;
    }
    /// Caster `id` this frame, one that never moves out of the cached depth
    /// (the plants, whose sway the overlays of the near levels carry):
    /// its change redraws the pages its old and new bounds touch, or, when
    /// it knows what changed since the generation it was last seen with
    /// (`changes`, given that generation: its cells come and go), the pages
    /// those boxes touch.
    using Changes = std::function<bool(uint64_t since, std::vector<bromath::AABB3>& boxes)>;
    void still(uint32_t id, uint64_t generation, const bromath::AABB3& bounds, ShadowPages& pages,
               const Changes& changes = {}) {
        auto [it, added] = states_.try_emplace(id);
        State& s = it->second;
        std::vector<bromath::AABB3> boxes;
        if (added) {
            pages.invalidate(bounds);
        } else if (generation != s.generation && changes && changes(s.generation, boxes)) {
            for (const bromath::AABB3& b : boxes) pages.invalidate(b);
        } else if (generation != s.generation) {
            pages.invalidate(s.bounds);
            pages.invalidate(bounds);
        }
        s.generation = generation;
        s.bounds = bounds;
        s.moving = false;
        s.frame = frame_;
    }
    /// The frame's casters are in: those gone take their depth with them.
    void end(ShadowPages& pages) {
        for (auto it = states_.begin(); it != states_.end();) {
            if (it->second.frame == frame_) {
                ++it;
                continue;
            }
            if (!it->second.moving) pages.invalidate(it->second.bounds);
            it = states_.erase(it);
        }
    }
    bool moving(uint32_t id) const {
        const auto it = states_.find(id);
        return it != states_.end() && it->second.moving;
    }
    void clear() { states_.clear(); }

private:
    struct State {
        uint64_t generation = 0;
        bromath::AABB3 bounds;
        bool moving = false;
        bool sways = false;
        int still = 0;
        uint64_t frame = 0;
    };
    std::unordered_map<uint32_t, State> states_;
    uint64_t frame_ = 0;
};

}  // namespace bro::scene
