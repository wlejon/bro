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
    const double u = (x + half_) / c + 0.5, v = (z + half_) / c + 0.5;
    const int i = std::clamp(static_cast<int>(std::floor(u)), 0, n);
    const int j = std::clamp(static_cast<int>(std::floor(v)), 0, n);
    const double fx = std::clamp(u - i, 0.0, 1.0), fz = std::clamp(v - j, 0.0, 1.0);
    const float* r = values + static_cast<size_t>(std::clamp(root, 0, roots() - 1)) * rootTexels();
    const auto at = [&](int a, int b) { return static_cast<double>(r[static_cast<size_t>(b) * s + a]); };
    const double lo = at(i, j) + (at(i + 1, j) - at(i, j)) * fx;
    const double hi = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fx;
    return static_cast<float>(lo + (hi - lo) * fz);
}

float LandGrid::sampleDir(const float* values, const double dir[3]) const {
    const ChartPoint p = chartOf(body_, dir);
    return sample(values, p.root, p.x, p.z);
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
