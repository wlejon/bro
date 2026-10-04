#include "scene/clipmap_terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bro::scene {

namespace {

// Must match CM_FADE in clipmap.vert.glsl / clipmap.frag.glsl.
constexpr float kFade = 0.08f;

float smoothstep01(float edge1, float x) {
    float t = std::clamp(x / edge1, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// CPU mirror of the procedural detail.
//
// Mirrors cmDetail() in clipmap_detail.glsl — same hash, same quintic fade,
// same octave amplitudes — with two deliberate differences:
//
//   * No band limit. The shader fades octaves out against the rendered cell
//     size, a screen-space quantity this query has no business knowing. A
//     collision query wants the surface as it exists, so every octave counts.
//     Near the camera the two agree anyway: that is where the rendered cell is
//     smallest and every octave is at full strength, and near the camera is the
//     only place anything collides.
//   * Doubles for the lattice coordinate, which makes the shader's anchoring
//     trick unnecessary.
// ---------------------------------------------------------------------------

uint32_t detailHash(int32_t cx, int32_t cz) {
    uint32_t h = static_cast<uint32_t>(cx + 0x2000000) * 0x8da6b343u
               + static_cast<uint32_t>(cz + 0x2000000) * 0xd8163841u;
    h ^= h >> 15; h *= 0x2c1b3c6du;
    h ^= h >> 12; h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

void detailGradient(int32_t cx, int32_t cz, float& gx, float& gz) {
    const float a = static_cast<float>(detailHash(cx, cz) & 0xffffu)
                  * (6.2831853f / 65536.0f);
    gx = std::cos(a);
    gz = std::sin(a);
}

float detailNoise(double px, double pz) {
    const double flx = std::floor(px), flz = std::floor(pz);
    const auto ix = static_cast<int32_t>(flx);
    const auto iz = static_cast<int32_t>(flz);
    const float fx = static_cast<float>(px - flx);
    const float fz = static_cast<float>(pz - flz);

    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uz = fz * fz * fz * (fz * (fz * 6.0f - 15.0f) + 10.0f);

    auto corner = [&](int dx, int dz) {
        float gx, gz;
        detailGradient(ix + dx, iz + dz, gx, gz);
        return gx * (fx - static_cast<float>(dx))
             + gz * (fz - static_cast<float>(dz));
    };
    const float va = corner(0, 0), vb = corner(1, 0);
    const float vc = corner(0, 1), vd = corner(1, 1);

    return va + (vb - va) * ux + (vc - va) * uz
              + (va - vb - vc + vd) * ux * uz;
}

} // namespace

// ---------------------------------------------------------------------------
// CPU height query — must agree with the GPU or things fall through the floor
// ---------------------------------------------------------------------------

float ClipmapTerrain::baseElevationAt(float x, float z) const {
    // Mirrors cmHeight() in the shaders exactly, except that it always samples
    // mip level 0 (bilinear) — there is no CPU mip chain. Same layer order,
    // same coverage weights, same coarse-to-fine blend.
    //
    // cubicHeight does NOT change this, and that is the answer rather than the
    // omission. This query is camera-free — a collision height that moved when
    // the camera moved would be a worse defect than any filter fixes — and the
    // shader's cubic path is gated on a screen-space quantity (pixels per
    // sampled texel) that is an exact 0 in the near field, where this mirror is
    // exact and where things actually stand. Past that gate the mirror is
    // already approximate for the mip it cannot model; the cubic adds at most
    // 1/6 of the field's second difference over one sampled texel on top, in
    // the same place. See clipmap_cubic_height.glsl, THE CPU MIRRORS.
    auto sample = [&](const ClipmapLayer& l, float& w) -> float {
        if (!l.present || l.width < 1 || l.height < 1 || l.data.empty()) {
            w = 0.0f;
            return 0.0f;
        }
        const float tx = (x - l.originX) / l.metresPerCell;
        const float tz = (z - l.originZ) / l.metresPerCell;
        const float ux = (tx + 0.5f) / static_cast<float>(l.width);
        const float uz = (tz + 0.5f) / static_cast<float>(l.height);
        // cmEdge: a periodic layer has no east-west edge to be near.
        w = l.wrapX
            ? smoothstep01(kFade, std::min(uz, 1.0f - uz))
            : smoothstep01(kFade, std::min(std::min(ux, 1.0f - ux),
                                           std::min(uz, 1.0f - uz)));

        // GL_LINEAR at level 0, in texel space: GL_REPEAT in S when the layer
        // wraps, GL_CLAMP_TO_EDGE otherwise, and always clamped in T.
        const int x0 = static_cast<int>(std::floor(tx));
        const int z0 = static_cast<int>(std::floor(tz));
        const float fx = tx - static_cast<float>(x0);
        const float fz = tz - static_cast<float>(z0);
        auto at = [&](int ix, int iz) -> float {
            ix = l.wrapX ? ((ix % l.width) + l.width) % l.width
                         : std::clamp(ix, 0, l.width - 1);
            iz = std::clamp(iz, 0, l.height - 1);
            return l.data[static_cast<size_t>(iz) * l.width + ix];
        };
        const float h00 = at(x0, z0),     h10 = at(x0 + 1, z0);
        const float h01 = at(x0, z0 + 1), h11 = at(x0 + 1, z0 + 1);
        return (h00 * (1.0f - fx) + h10 * fx) * (1.0f - fz)
             + (h01 * (1.0f - fx) + h11 * fx) * fz;
    };

    const int n = layerCount_;
    float w = 0.0f;
    float h = 0.0f;
    if      (n > 5) h = sample(layers_[5], w);
    else if (n > 4) h = sample(layers_[4], w);
    else if (n > 3) h = sample(layers_[3], w);
    else if (n > 2) h = sample(layers_[2], w);
    else if (n > 1) h = sample(layers_[1], w);
    else if (n > 0) h = sample(layers_[0], w);
    if (n > 5) { float s = sample(layers_[4], w); h = h + (s - h) * w; }
    if (n > 4) { float s = sample(layers_[3], w); h = h + (s - h) * w; }
    if (n > 3) { float s = sample(layers_[2], w); h = h + (s - h) * w; }
    if (n > 2) { float s = sample(layers_[1], w); h = h + (s - h) * w; }
    if (n > 1) { float s = sample(layers_[0], w); h = h + (s - h) * w; }

    return cfg_.seaLevel + cfg_.heightScale * h;
}

// Mirrors cmDataFloor() — the finest cell the DATA resolves here, blended in
// log2 with the same coverage weights baseElevationAt uses. It sets where the
// procedural band starts, so it has to agree with the shader or the surface a
// query reports stops being the surface the GPU draws.
float ClipmapTerrain::dataFloorAt(float x, float z) const {
    auto cover = [&](const ClipmapLayer& l) -> float {
        if (!l.present || l.width < 1 || l.height < 1 || l.data.empty()) return 0.0f;
        const float ux = ((x - l.originX) / l.metresPerCell + 0.5f)
                       / static_cast<float>(l.width);
        const float uz = ((z - l.originZ) / l.metresPerCell + 0.5f)
                       / static_cast<float>(l.height);
        if (l.wrapX) return smoothstep01(kFade, std::min(uz, 1.0f - uz));
        return smoothstep01(kFade, std::min(std::min(ux, 1.0f - ux),
                                            std::min(uz, 1.0f - uz)));
    };
    const int n = layerCount_;
    float f = 0.0f;
    if      (n > 5) f = std::log2(layers_[5].metresPerCell);
    else if (n > 4) f = std::log2(layers_[4].metresPerCell);
    else if (n > 3) f = std::log2(layers_[3].metresPerCell);
    else if (n > 2) f = std::log2(layers_[2].metresPerCell);
    else if (n > 1) f = std::log2(layers_[1].metresPerCell);
    else if (n > 0) f = std::log2(layers_[0].metresPerCell);
    else return cfg_.cellSize;
    if (n > 5) { float w = cover(layers_[4]); f += (std::log2(layers_[4].metresPerCell) - f) * w; }
    if (n > 4) { float w = cover(layers_[3]); f += (std::log2(layers_[3].metresPerCell) - f) * w; }
    if (n > 3) { float w = cover(layers_[2]); f += (std::log2(layers_[2].metresPerCell) - f) * w; }
    if (n > 2) { float w = cover(layers_[1]); f += (std::log2(layers_[1].metresPerCell) - f) * w; }
    if (n > 1) { float w = cover(layers_[0]); f += (std::log2(layers_[0].metresPerCell) - f) * w; }
    return std::exp2(f);
}

float ClipmapTerrain::elevationAt(float x, float z) const {
    const float h0 = baseElevationAt(x, z);

    const float e  = cfg_.cellSize;
    const float hx = baseElevationAt(x + e, z);
    const float hz = baseElevationAt(x, z + e);

    // cmSlopeFrom, verbatim: normalize(h0-hx, e, h0-hz).y, then 1 - that.
    const float dx = h0 - hx, dz = h0 - hz;
    const float len = std::sqrt(dx * dx + e * e + dz * dz);
    const float slope = std::clamp(1.0f - e / std::max(len, 1e-6f), 0.0f, 1.0f);
    const float weight = std::max(slope, 0.12f);   // cmDetailWeight

    // The band starts above cfg_.detailWavelength wherever the data is coarser
    // than it — see cmDetail. The high-pass against the data floor is mirrored;
    // the shader's low-pass against the rendered cell still is not, for the
    // reason above. This is the surface a caller stands the camera and its
    // collision on — it must be the drawn surface exactly.
    const float floorM = dataFloorAt(x, z);
    const int   up     = kDetailUpOctaves;
    double lambda = static_cast<double>(cfg_.detailWavelength)
                  * std::exp2(static_cast<double>(up));
    float  sum    = 0.0f;
    const int n = up + std::min(cfg_.detailOctaves, 8);
    for (int i = 0; i < n; ++i) {
        const float wDat = 1.0f - smoothstep01(2.0f * floorM,
                                               static_cast<float>(lambda) - 2.0f * floorM);
        if (wDat > 0.0f) {
            const float gain = std::pow(cfg_.detailGain, std::max(0, i - up));
            const float amp = cfg_.detailRelief * static_cast<float>(lambda) * gain;
            sum += amp * wDat * detailNoise(x / lambda, z / lambda);
        }
        lambda *= 0.5;
    }

    return h0 + weight * sum;
}

// ---------------------------------------------------------------------------
// Chart-aware ground — where the RENDERED sheet is, not where the field is.
//
// cmCurve (clipmap_common.glsl) maps the flat-chart point at arc distance
// d = R*th from the chart centre to
//     chord  rho = (R + h) * sin th                (horizontal, from centre)
//     height y   = h * cos th - 2R * sin^2(th/2)
// With the default camera-following centre the bend under any point the
// engine asks about is re-zeroed every update; with a PINNED centre
// (setChartCenter) a world position at chord rho sits over the flat point at
// arc d = R * asin(rho / (R + h)), and the sheet there has dropped by the
// sagitta 2R*sin^2(th/2) ~ rho^2/2R. Both R and the mapping are the very
// ones the shader uses (cfg_.planetRadius is pushed as u_planetRadius), so
// the two cannot drift.
//
// The inversion needs h before it has found the point that carries h, so it
// runs twice: h/R < ~1.3e-3 on any Earth-like world, so the h = 0 pass lands
// within rho * h/R (~500 m at 400 km) and the refined pass within
// centimetres of the true foot point.
// ---------------------------------------------------------------------------

bool ClipmapTerrain::chartPointUnder(float x, float z, float& fx, float& fz,
                                     float& th) const {
    fx = x;
    fz = z;
    th = 0.0f;
    if (!chartPinned_ || cfg_.planetRadius <= 0.0f) return false;
    const double R  = cfg_.planetRadius;
    const double dx = static_cast<double>(x) - chartX_;
    const double dz = static_cast<double>(z) - chartZ_;
    const double rho = std::sqrt(dx * dx + dz * dz);
    if (!(rho > 0.0)) return false;
    const double ux = dx / rho, uz = dz / rho;
    // asin's argument is clamped: a chord past R + h has bent beyond the
    // sphere's equator and has no point beneath it — answer with the rim.
    double t = std::asin(std::min(rho / R, 1.0));
    const double h0 = baseElevationAt(static_cast<float>(chartX_ + ux * R * t),
                                      static_cast<float>(chartZ_ + uz * R * t));
    t = std::asin(std::min(rho / std::max(R + h0, 1.0), 1.0));
    fx = static_cast<float>(chartX_ + ux * R * t);
    fz = static_cast<float>(chartZ_ + uz * R * t);
    th = static_cast<float>(t);
    return true;
}

float ClipmapTerrain::renderedElevationAt(float x, float z) const {
    float fx, fz, th;
    if (!chartPointUnder(x, z, fx, fz, th)) return elevationAt(x, z);
    const double h = elevationAt(fx, fz);
    const double s = std::sin(0.5 * static_cast<double>(th));
    return static_cast<float>(h * std::cos(static_cast<double>(th))
                              - 2.0 * static_cast<double>(cfg_.planetRadius)
                                    * s * s);
}

float ClipmapTerrain::detailBound() const {
    // Worst case over the whole world, so it assumes the band has climbed as
    // far as it can: the upward octaves are only live where the data is coarse,
    // but a cull margin has to hold everywhere.
    float lambda = cfg_.detailWavelength * std::exp2(float(kDetailUpOctaves));
    float sum    = 0.0f;
    const int n = kDetailUpOctaves + std::min(cfg_.detailOctaves, 8);
    for (int i = 0; i < n; ++i) {
        sum += cfg_.detailRelief * lambda
             * std::pow(cfg_.detailGain, std::max(0, i - kDetailUpOctaves));
        lambda *= 0.5f;
    }
    return sum;
}

} // namespace bro::scene
