#include "scene/world/land_sun.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

// How badly a level wants making, the world service's pixels: a level whose
// square left the eye or whose land changed shows the wrong shade where it
// matters most; a turned sun moves every shadow a little; refined tiles
// sharpen what is there (a level first made over the coarse tiles of a
// view's first frames stands their heights over the ground's hollows, and
// shades them). A want doubles every kRefreshFrames it waits, up to
// kMaxError: the service grants the dearest errors first, and a level
// outranked frame after frame would never be made again.
constexpr float kMovedError = 8.0f;
constexpr float kLandError = 8.0f;
constexpr float kSunError = 4.0f;
constexpr float kTilesError = 1.0f;
constexpr float kMaxError = 64.0f;

}  // namespace

void LandSun::update(const bromath::Vec3& eye, const bromath::Vec3& sun, uint64_t generation, bool tilesChanged) {
    ++frame_;
    sun_ = sun;
    generation_ = generation;
    wants_.clear();
    for (int l = 0; l < kLevels; ++l) {
        State& s = state_[static_cast<size_t>(l)];
        if (tilesChanged) s.changed = true;
        const double texel = static_cast<double>(kTexel0) * std::ldexp(1.0, l);
        const double snap = texel * kSnapTexels;
        s.cx = std::floor(eye.x / snap + 0.5) * snap;
        s.cz = std::floor(eye.z / snap + 0.5) * snap;
        const double half = 0.5 * kSide * texel;
        Want w;
        w.level = l;
        if (s.shown.layer < 0) {
            w.required = true;
            w.error = kMovedError;
        } else if (s.shown.x0 != s.cx - half || s.shown.z0 != s.cz - half) {
            w.error = kMovedError;
        } else if (s.generation != generation) {
            w.error = kLandError;
        } else if (bromath::vdot(s.sun, sun) < kSunTurnCos) {
            w.error = kSunError;
        } else if (s.changed && frame_ - s.madeFrame >= static_cast<uint64_t>(kRefreshFrames)) {
            w.error = kTilesError;
        } else {
            s.wantedFrame = 0;
            continue;
        }
        if (s.wantedFrame == 0) s.wantedFrame = frame_;
        const float waited = static_cast<float>(frame_ - s.wantedFrame) / static_cast<float>(kRefreshFrames);
        w.error = std::min(w.error * std::exp2(waited), kMaxError);
        wants_.push_back(w);
    }
}

LandSun::Level LandSun::make(int l) {
    State& s = state_[static_cast<size_t>(l)];
    const float texel = kTexel0 * std::ldexp(1.0f, l);
    const double half = 0.5 * kSide * static_cast<double>(texel);
    Level made;
    made.x0 = s.cx - half;
    made.z0 = s.cz - half;
    made.texel = texel;
    // The copy not shown (level l's are layers 2l and 2l + 1).
    made.layer = 2 * l + (s.shown.layer == 2 * l ? 1 : 0);
    s.shown = made;
    s.sun = sun_;
    s.generation = generation_;
    s.changed = false;
    s.madeFrame = frame_;
    s.wantedFrame = 0;
    return made;
}

void LandSun::forget() {
    state_ = {};
    wants_.clear();
}

}  // namespace bro::scene
