#pragma once

// The scene's wind field: a steady wind whose direction drifts slowly, with
// gusts that travel downwind as bands. The field itself is one brokernel
// source, shaders/common/wind.bk: vegetation (and later grass and particles)
// bends with it on the GPU, and scripts read it with scene.windAt(), which
// evaluates the same functions through brokernel's CPU build (windAt below).
// This header is the host's half: what setWind sets, and the field's state
// at a moment, which the camera block carries to the shaders.
//
// It sits beside the legacy wind (setWind's direction / strength /
// frequency: a single sine on vertex colour R), which it leaves alone.

#include <algorithm>
#include <cmath>

namespace bro::scene {

/// What setWind sets beyond the legacy fields. Speeds in m/s, scale in m.
struct WindFieldParams {
    float speed = 2.5f;       // the steady wind (it breathes +-28% over ~3 min)
    float gust = 1.0f;        // gust amount: 0 = none, 1 = a gusty day, 2 = a gale's
    float gustSpeed = 9.0f;   // how fast the gust bands travel downwind
    float gustScale = 45.0f;  // a band's length along the wind (1.85x that across it)
    float drift = 0.5f;       // radians the direction wanders either side of the set one
};

/// The field at one moment: what the camera block carries
/// (wind_field / wind_gust in common/scene.bk's Camera) plus the clock.
struct WindFieldState {
    float dirX = 1.0f, dirZ = 0.0f; // horizontal direction, unit, drifting
    float base = 0.0f;              // the steady speed now
    float gustSpeed = 9.0f;
    float gust = 0.0f;              // gust amount
    float invAlong = 1.0f / 45.0f;  // 1 / band length along the wind
    float invAcross = 1.0f / 83.25f;
    float time = 0.0f;
};

/// The state at time `t` for wind set toward `dir` (only its x/z count; a
/// vertical or zero direction blows along +x).
inline WindFieldState windFieldState(const float dir[3], const WindFieldParams& p, float t) {
    WindFieldState s;
    float dx = dir[0], dz = dir[2];
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len > 1e-6f) { dx /= len; dz /= len; } else { dx = 1.0f; dz = 0.0f; }
    const float ang = std::atan2(dz, dx) +
        p.drift * (0.7f * std::sin(t * 0.021f) + 0.3f * std::sin(t * 0.063f + 1.0f));
    s.dirX = std::cos(ang);
    s.dirZ = std::sin(ang);
    s.base = std::max(0.0f, p.speed) * (1.0f + 0.28f * std::sin(t * 0.037f + 2.0f));
    s.gustSpeed = p.gustSpeed;
    s.gust = std::max(0.0f, p.gust);
    const float scale = std::max(1.0f, p.gustScale);
    s.invAlong = 1.0f / scale;
    s.invAcross = 1.0f / (scale * 1.85f);
    s.time = t;
    return s;
}

/// True when nothing in the field moves (no steady wind, no gusts).
inline bool windFieldCalm(const WindFieldState& s) { return s.base <= 0.0f && s.gust <= 0.0f; }

/// The farthest sway (shaders/common/vegetation.bk) can move any
/// point of a plant `height` tall (world units) under `p`, at the strongest
/// steady wind and the strongest gust: the culling and shadow-fit pad. The
/// bound follows the shader's terms one by one, plus a leaf's flutter.
inline float windSwayReach(const WindFieldParams& p, float height) {
    if (height <= 0.0f) return 0.0f;
    const float base = std::max(0.0f, p.speed) * 1.28f;
    // (0.5 + 0.85 * 0.9 - 0.45) * 2.4 = 1.96 at most (wind_simplex stays
    // within +-0.9), padded.
    const float gu = 2.16f * std::max(0.0f, p.gust);
    const float calm = std::min(base / 2.5f, 3.0f);
    const float spd = base + 4.0f * gu;
    const float trunk = (spd * 0.006f + gu * 0.03f + 0.008f * (0.6f * calm + gu) + 0.004f * (0.5f * calm + gu)) *
                        height * 1.69f;
    const float limb = (0.09f * calm + 0.16f * gu + 0.07f * (0.35f * calm + gu) + 0.05f * (0.4f * calm + gu)) *
                       (0.5f + 0.06f * height);
    return trunk * (1.0f + trunk * 0.1f / std::max(height, 1.0f)) + limb + 0.25f;
}

/// The oscillator clock's period (s): common/scene.bk's
/// WIND_CLOCK_PERIOD, the camera block's windParams.w.
inline constexpr double kWindClockPeriod = 256.0;

/// Wind velocity (m/s, horizontal) and gust at absolute world (x, z):
/// wind.bk's wind_velocity, run on the CPU (its build is made once, on the
/// first call). All zero when the kernel could not be built (logged).
struct WindSample { float velocity[3]; float gust; float speed; };
WindSample windAt(const WindFieldState& s, float x, float z);

}  // namespace bro::scene
