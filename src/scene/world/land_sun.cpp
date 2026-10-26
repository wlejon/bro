#include "scene/world/land_sun.h"

#include "scene/world/world_service.h"

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

// How badly a level wants making, the world service's pixels: a level whose
// square left the eye or whose land changed shows the wrong shade where it
// matters most; a turned sun moves every shadow a little. Refined tiles are
// as bad as a changed land: a level made over coarser tiles stands their
// heights over the ground's hollows and shades them, the whole view where
// the eye came down from high over it (the planet's nadir from 6 km, dark
// for 20 frames under a level made from 90 km, and at 1 px never said to
// be pending). A want doubles every kRefreshFrames it waits, up to
// kMaxError: the service grants the dearest errors first, and a level
// outranked frame after frame would never be made again.
constexpr float kMovedError = 8.0f;
constexpr float kLandError = 8.0f;
constexpr float kSunError = 4.0f;
constexpr float kTilesError = 8.0f;
constexpr float kMaxError = 64.0f;

}  // namespace

void LandSun::update(const bromath::Vec3& eye, const bromath::Vec3& sun, uint64_t generation, bool tilesChanged,
                     float pixelsPerMetre, float above, float canopyRange) {
    ++frame_;
    sun_ = sun;
    eyeY_ = eye.y;
    above_ = above;
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
        // (What a moved level leaves wrong: the coarser level's texel at the
        // nearest ground.)
        const float coarser = static_cast<float>(2.0 * texel) * pixelsPerMetre;
        // (Risen or fallen a tenth of the canopy's cast range, where its
        // fade could be in reach of the eye then or now: past it, all of
        // the canopy shades. Every snap of height made the planet's descent
        // remake the finest level 15 times a second, ahead of its tiles.)
        const bool rose = canopyRange > 0.0f && std::min(above, s.madeAbove) < canopyRange &&
                          std::fabs(eye.y - s.madeY) > 0.1 * canopyRange;
        Want w;
        w.level = l;
        if (s.shown.layer < 0) {
            w.required = true;
            w.error = kMovedError;
        } else if (s.shown.x0 != s.cx - half || s.shown.z0 != s.cz - half || rose) {
            w.error = std::min(kMovedError, coarser);
        } else if (s.generation != generation) {
            w.error = kLandError;
        } else if (bromath::vdot(s.sun, sun) < kSunTurnCos) {
            w.error = kSunError;
        } else if (s.changed && (frame_ - s.madeFrame >= static_cast<uint64_t>(kTilesRefreshFrames) ||
                                 WorldService::settlingAll())) {
            // (Held back only to spare the frames of a moving eye: a
            // settle, everything in now, waits for none.)
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
    s.madeY = eyeY_;
    s.madeAbove = above_;
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
