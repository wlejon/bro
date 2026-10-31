#pragma once

// The scene's wind field: a steady wind whose direction drifts slowly, with
// gusts that travel downwind as bands. Vegetation (and later grass and
// particles) bends with it on the GPU; scripts read the same field with
// scene.windAt(). This header is the CPU twin of wind_at() /
// wind_gust_at() in shaders/common/scene.bk: the same integer-hash value
// noise and the same formulas in float, so a script and a shader agree on
// where a gust is.
//
// It sits beside the legacy wind (setWind's direction / strength /
// frequency: a single sine on vertex colour R), which it leaves alone.

#include <algorithm>
#include <cmath>
#include <cstdint>

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
    // (0.5 + 0.85 * 0.9 - 0.45) * 2.4 = 1.96 at most (windSimplex stays
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

/// Integer-hash value noise in [-1, 1] (sceneWindNoise).
inline float windHash(int32_t x, int32_t y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return static_cast<float>(h) / 4294967296.0f;
}
inline float windNoise(float px, float py) {
    const float ix = std::floor(px), iy = std::floor(py);
    const float fx = px - ix, fy = py - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    const int32_t x = static_cast<int32_t>(ix), y = static_cast<int32_t>(iy);
    const float a = windHash(x, y), b = windHash(x + 1, y);
    const float c = windHash(x, y + 1), d = windHash(x + 1, y + 1);
    const float ab = a + (b - a) * ux, cd = c + (d - c) * ux;
    return (ab + (cd - ab) * uy) * 2.0f - 1.0f;
}

/// Simplex gradient noise, about [-0.8, 0.8] with the value noise's spread
/// (sceneWindSimplex): the gusts. No plateaus at the lattice points, so a
/// gust band does not trace the lattice; smooth to its second derivative.
inline float windSimplexCorner(int32_t x, int32_t y, float dx, float dy) {
    float t = 0.5f - dx * dx - dy * dy;
    if (t <= 0.0f) return 0.0f;
    const float a = windHash(x, y) * 6.2831853f;
    t *= t;
    return t * t * (std::cos(a) * dx + std::sin(a) * dy);
}
inline constexpr float kWindSimplexGrid = 0.38f;   // SCENE_WIND_SIMPLEX_GRID
inline float windSimplex(float px, float py) {
    constexpr float F2 = 0.36602540378f, G2 = 0.21132486540f;
    px *= kWindSimplexGrid;
    py *= kWindSimplexGrid;
    const float ix = std::floor(px + (px + py) * F2), iy = std::floor(py + (px + py) * F2);
    const float d0x = px - ix + (ix + iy) * G2, d0y = py - iy + (ix + iy) * G2;
    const int32_t ox = d0x > d0y ? 1 : 0, oy = 1 - ox;
    const int32_t x = static_cast<int32_t>(ix), y = static_cast<int32_t>(iy);
    return 80.0f * (windSimplexCorner(x, y, d0x, d0y) +
                    windSimplexCorner(x + ox, y + oy, d0x - ox + G2, d0y - oy + G2) +
                    windSimplexCorner(x + 1, y + 1, d0x - 1.0f + 2.0f * G2, d0y - 1.0f + 2.0f * G2));
}

/// max(x, 0) with its corner rounded by a hyperbola (sceneWindSoftRamp): a
/// gust starts with no kink and no jump in its acceleration.
inline constexpr float kWindGustSoft = 0.06f;
inline float windSoftRamp(float x) { return 0.5f * (x + std::sqrt(x * x + kWindGustSoft * kWindGustSoft)); }

/// The oscillator clock's period (s): common/scene.bk's
/// WIND_CLOCK_PERIOD, the camera block's windParams.w.
inline constexpr double kWindClockPeriod = 256.0;

/// The gust at absolute world (x, z): 0 between bands, up to ~1.7 x gust in one
/// (its mean over the field 0.34 x gust).
inline float windGustAt(const WindFieldState& s, float x, float z) {
    if (s.gust <= 0.0f) return 0.0f;
    const float along = x * s.dirX + z * s.dirZ - s.time * s.gustSpeed;
    const float across = -x * s.dirZ + z * s.dirX;
    const float g = windSimplex(along * s.invAlong, across * s.invAcross) * 0.5f + 0.5f +
                    0.35f * windSimplex(along * s.invAlong * 3.18f + 5.3f, across * s.invAcross * 4.17f);
    return windSoftRamp(g - 0.45f) * 2.4f * s.gust;
}

/// Wind velocity (m/s, horizontal) and gust at absolute world (x, z).
struct WindSample { float velocity[3]; float gust; float speed; };
inline WindSample windAt(const WindFieldState& s, float x, float z) {
    WindSample w{};
    w.gust = windGustAt(s, x, z);
    const float spd = s.base + w.gust * 4.0f;
    const float wobble = windNoise((x * s.dirX + z * s.dirZ) * 0.03f - s.time * 0.3f, s.time * 0.1f) * 0.25f;
    w.velocity[0] = (s.dirX - s.dirZ * wobble) * spd;
    w.velocity[1] = 0.0f;
    w.velocity[2] = (s.dirZ + s.dirX * wobble) * spd;
    w.speed = std::sqrt(w.velocity[0] * w.velocity[0] + w.velocity[2] * w.velocity[2]);
    return w;
}

}  // namespace bro::scene
