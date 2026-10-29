#include "scene/world/land_grid.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bro::scene {

int LandProcess::iterations() const {
    int n = 0;
    for (const Pass& p : passes) n += std::max(p.iterations, 0);
    return n;
}

void LandGrid::configure(const Body& body, double extent, int resolution) {
    body_ = body;
    radius_ = body.flat() ? 0.0 : body.radius;
    half_ = body.flat() ? extent : faceHalf(body);
    cells_ = std::clamp(resolution, 1, LandProcess::kMaxResolution);
    buildHalo();
}

void LandGrid::buildHalo() {
    halo_.clear();
    const int n = cells_, s = side();
    const auto texel = [&](int root, int x, int z) {
        return static_cast<uint32_t>((static_cast<size_t>(root) * s + z) * s + x);
    };
    // The cell holding halo cell (i, j)'s centre: on a flat body the edge's;
    // on a round one the cell of the face the centre lies on that holds it.
    const auto nearest = [&](int root, int i, int j) {
        if (!round()) return texel(root, std::clamp(i, 1, n), std::clamp(j, 1, n));
        const double c = cell();
        double d[3];
        dirOf(body_, {root, -half_ + (i - 0.5) * c, -half_ + (j - 0.5) * c}, d);
        const ChartPoint p = chartOf(body_, d);
        const int si = std::clamp(static_cast<int>(std::floor((p.x + half_) / c)) + 1, 1, n);
        const int sj = std::clamp(static_cast<int>(std::floor((p.z + half_) / c)) + 1, 1, n);
        return texel(p.root, si, sj);
    };
    for (int root = 0; root < roots(); ++root)
        for (int j = 0; j < s; ++j)
            for (int i = 0; i < s; ++i) {
                const bool outI = i == 0 || i > n, outJ = j == 0 || j > n;
                if (!outI && !outJ) continue;
                const uint32_t dst = texel(root, i, j);
                if (!(outI && outJ)) {
                    const uint32_t src = nearest(root, i, j);
                    halo_.push_back({dst, {src, src, src}});
                    continue;
                }
                // A corner: the face's corner cell and the halo's two cells
                // beside this one (the neighbouring faces' corner cells on a
                // round body; on a flat one all three the corner cell).
                const int ci = std::clamp(i, 1, n), cj = std::clamp(j, 1, n);
                halo_.push_back({dst, {texel(root, ci, cj), nearest(root, i, cj), nearest(root, ci, j)}});
            }
}

float LandGrid::sample(const float* values, int root, double x, double z) const {
    if (!values || cells_ <= 0) return 0.0f;
    const int n = cells_, s = side();
    const double c = cell();
    // Texel i's centre is at -half + (i - 0.5) cells.
    // (The cubic B-spline over the four centres about it each way, as
    // shaders/ground/land_grid.glsl's landGridAt.)
    const double u = std::clamp((x + half_) / c + 0.5, 0.0, n + 1.0), v = std::clamp((z + half_) / c + 0.5, 0.0, n + 1.0);
    const int i = static_cast<int>(std::floor(u)), j = static_cast<int>(std::floor(v));
    double wx[4], wz[4];
    landBspline(u - i, wx);
    landBspline(v - j, wz);
    const float* r = values + static_cast<size_t>(std::clamp(root, 0, roots() - 1)) * rootTexels();
    double sum = 0.0;
    for (int b = 0; b < 4; ++b) {
        const size_t row = static_cast<size_t>(std::clamp(j - 1 + b, 0, n + 1)) * s;
        double acc = 0.0;
        for (int a = 0; a < 4; ++a) acc += wx[a] * r[row + std::clamp(i - 1 + a, 0, n + 1)];
        sum += wz[b] * acc;
    }
    return static_cast<float>(sum);
}

float LandGrid::sampleDir(const float* values, const double dir[3]) const {
    const ChartPoint p = chartOf(body_, dir);
    // Within a cell of a face's edge the faces about the point cross-faded,
    // each by how far inside its square it lies (the B-spline reaches two
    // cells about a point, the halo one past the face; as land_grid.glsl's
    // landGridFaceWeight).
    const double c = cell();
    const auto weight = [&](const ChartPoint& q) {
        const double m = std::max(std::fabs(q.x), std::fabs(q.z));
        const double t = std::clamp((m - (half_ - c)) / (2.0 * c), 0.0, 1.0);
        return 1.0 - t * t * (3.0 - 2.0 * t);
    };
    if (!round() || std::max(std::fabs(p.x), std::fabs(p.z)) < half_ - c) return sample(values, p.root, p.x, p.z);
    double sum = 0.0, wsum = 0.0;
    for (int k = 0; k < 6; ++k) {
        // (Face k's normal: +x, -x, +y, -y, +z, -z.)
        const double n = (k & 1 ? -1.0 : 1.0) * dir[k >> 1];
        if (k != p.root && n < 0.2) continue;
        const ChartPoint q = k == p.root ? p : chartOf(body_, dir, k);
        const double w = weight(q);
        if (w <= 0.0) continue;
        sum += w * sample(values, k, q.x, q.z);
        wsum += w;
    }
    return wsum > 0.0 ? static_cast<float>(sum / wsum) : sample(values, p.root, p.x, p.z);
}

void LandBase::global(int root, double x, double z, float out[4]) const {
    out[0] = values.empty() ? 0.0f : grid.sample(values.data(), root, x, z);
    out[1] = scales.empty() ? 1.0f : grid.sample(scales.data(), root, x, z);
    out[2] = own[0].empty() ? 0.0f : grid.sample(own[0].data(), root, x, z);
    out[3] = own[1].empty() ? 0.0f : grid.sample(own[1].data(), root, x, z);
}

void LandBase::field(int root, double x, double z, float out[4], const LandRegionSet* in, float* rests) const {
    if (grid.round()) {
        double d[3];
        dirOf(grid.body(), {root, x, z}, d);
        fieldDir(d, out, in, rests);
        return;
    }
    global(root, x, z, out);
    if (rests) std::fill(rests, rests + LandRegionConfig::kMaxLevels, 1.0f);
    if (!regions || !lattice.config.active()) return;
    if (in) {
        lattice.blendAt(*in, x, z, out, rests);
        return;
    }
    const auto set = regions->snapshot();
    lattice.blendAt(*set, x, z, out, rests);
}

float LandBase::standIn(const double dir[3], const float* rests, const float* values) const {
    const double R = grid.body().radius;
    float s = 0.0f;
    for (size_t L = 0; L < relief.size() && L < static_cast<size_t>(LandRegionConfig::kMaxLevels); ++L) {
        if (!relief[L] || !(rests[L] > 0.0f)) continue;
        s += rests[L] * (values ? values[L] : relief[L]->at(R * dir[0], R * dir[1], R * dir[2]));
    }
    return s;
}

float LandBase::standInAt(double x, double z, const float* rests) const {
    float s = 0.0f;
    for (size_t L = 0; L < relief.size() && L < static_cast<size_t>(LandRegionConfig::kMaxLevels); ++L)
        if (relief[L] && rests[L] > 0.0f) s += rests[L] * relief[L]->at(static_cast<float>(x), static_cast<float>(z));
    return s;
}

float LandBase::over(float tree, int root, double x, double z, const LandRegionSet* set, const float* values) const {
    float v[4], rests[LandRegionConfig::kMaxLevels];
    if (grid.round()) {
        double d[3];
        dirOf(grid.body(), {root, x, z}, d);
        fieldDir(d, v, set, rests);
        return v[0] + v[1] * (tree + (hasRelief() ? standIn(d, rests, values) : 0.0f));
    }
    field(root, x, z, v, set, rests);
    return v[0] + v[1] * (tree + (hasRelief() ? standInAt(x, z, rests) : 0.0f));
}

float LandBase::overDir(float tree, const double dir[3], const float* values) const {
    float v[4], rests[LandRegionConfig::kMaxLevels];
    fieldDir(dir, v, nullptr, rests);
    if (!hasRelief()) return v[0] + v[1] * tree;
    if (!grid.round()) {
        const ChartPoint p = chartOf(grid.body(), dir);
        return v[0] + v[1] * (tree + standInAt(p.x, p.z, rests));
    }
    return v[0] + v[1] * (tree + standIn(dir, rests, values));
}

void LandBase::fieldDir(const double dir[3], float out[4], const LandRegionSet* in, float* rests) const {
    const ChartPoint p = chartOf(grid.body(), dir);
    if (grid.round()) {
        // (The faces cross-faded at their edges, as the GPU's landGridAtDir.)
        out[0] = values.empty() ? 0.0f : grid.sampleDir(values.data(), dir);
        out[1] = scales.empty() ? 1.0f : grid.sampleDir(scales.data(), dir);
        out[2] = own[0].empty() ? 0.0f : grid.sampleDir(own[0].data(), dir);
        out[3] = own[1].empty() ? 0.0f : grid.sampleDir(own[1].data(), dir);
    } else {
        global(p.root, p.x, p.z, out);
    }
    if (rests) std::fill(rests, rests + LandRegionConfig::kMaxLevels, 1.0f);
    if (!regions || !lattice.config.active()) return;
    std::shared_ptr<const LandRegionSet> held;
    if (!in) {
        held = regions->snapshot();
        in = held.get();
    }
    if (grid.round())
        lattice.blendDir(*in, dir, out, rests);
    else
        lattice.blendAt(*in, p.x, p.z, out, rests);
}

int LandBase::finestWhole(const double dir[3], double x, double z, double margin) const {
    if (!regions || !lattice.config.active()) return 0;
    const auto set = regions->snapshot();
    const int levels = lattice.config.levels;
    const double R = grid.round() ? grid.body().radius : 0.0;
    for (int l = 0; l < levels; ++l) {
        const LandRegionCentre& c = set->centre[l];
        if (!c.active) continue;
        double d;
        if (grid.round()) {
            const double dx = dir[0] - c.at.dir[0], dy = dir[1] - c.at.dir[1], dz = dir[2] - c.at.dir[2];
            d = 2.0 * R * std::asin(std::min(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz), 1.0));
        } else {
            d = std::hypot(x - c.at.x, z - c.at.z);
        }
        if (d + margin <= c.inner) return l;
    }
    return levels;
}

std::string LandGrid::glslConfig(int channels) const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "#define LAND_GRID 1\n"
                  "#define LAND_GRID_CELLS %d\n"
                  "#define LAND_GRID_ROOTS %d\n"
                  "#define LAND_GRID_CHANNELS %d\n"
                  "%s"
                  "const float LAND_GRID_HALF = %.9g;\n"
                  "const float LAND_GRID_RADIUS = %.9g;\n",
                  cells_, roots(), std::clamp(channels, 1, LandProcess::kMaxChannels),
                  round() ? "#define LAND_GRID_ROUND 1\n" : "", half_, radius_);
    return buf;
}

}  // namespace bro::scene
