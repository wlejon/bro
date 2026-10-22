// The land's processes' grid (scene/world/land_grid.h): a flat body's one
// root and its halo repeating the edge's cells; a round body's six cube
// faces, each halo cell copied from the cell of the face its centre lies on
// that holds it; sampling a cell's value at its centre, bilinear between,
// continuous across a face's edge and a cube's corner; the shaders' layout.

#include "scene/world/land_grid.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bro::scene;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

constexpr double kEarth = 6371000.0;

void unit(double v[3]) {
    const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    for (int i = 0; i < 3; ++i) v[i] /= l;
}

// Every interior cell from `f` at its centre, then the halo's copies.
template <class F>
std::vector<float> fill(const LandGrid& g, F f) {
    std::vector<float> v(g.texels(), 0.0f);
    const int s = g.side();
    for (int r = 0; r < g.roots(); ++r)
        for (int j = 1; j <= g.cells(); ++j)
            for (int i = 1; i <= g.cells(); ++i)
                v[(static_cast<size_t>(r) * s + j) * s + i] =
                    static_cast<float>(f(r, -g.half() + (i - 0.5) * g.cell(), -g.half() + (j - 0.5) * g.cell()));
    for (const LandGrid::HaloCopy& h : g.halo()) v[h.dst] = (v[h.src[0]] + v[h.src[1]] + v[h.src[2]]) / 3.0f;
    return v;
}

void flat() {
    LandGrid g;
    g.configure(Body{}, 1000.0, 50);
    check(!g.round() && g.roots() == 1 && g.side() == 52 && std::fabs(g.cell() - 40.0) < 1e-9, "flat: one root");
    check(g.halo().size() == 4u * 50 + 4, "flat: a halo of 4N + 4 cells");
    bool edges = true;
    for (const LandGrid::HaloCopy& h : g.halo()) {
        const int dx = static_cast<int>(h.dst % 52), dz = static_cast<int>(h.dst / 52);
        const int sx = static_cast<int>(h.src[0] % 52), sz = static_cast<int>(h.src[0] / 52);
        edges = edges && h.src[1] == h.src[0] && h.src[2] == h.src[0];
        edges = edges && sx == std::clamp(dx, 1, 50) && sz == std::clamp(dz, 1, 50);
    }
    check(edges, "flat: the halo repeats the edge's cells");
    const auto f = [](int, double x, double z) { return 3.0 * x - 2.0 * z + 7.0; };
    const std::vector<float> v = fill(g, f);
    // A linear field is exact anywhere within the centres; held past them.
    double worst = 0.0;
    for (double x = -980.0; x <= 980.0; x += 37.3)
        for (double z = -980.0; z <= 980.0; z += 41.9) worst = std::max(worst, std::fabs(g.sample(v.data(), 0, x, z) - f(0, x, z)));
    check(worst < 1e-2, "flat: bilinear between centres (worst " + std::to_string(worst) + ")");
    check(std::fabs(g.sample(v.data(), 0, 2000.0, 0.0) - f(0, 980.0, 0.0)) < 1e-2, "flat: held past the last centre");
    const std::string glsl = g.glslConfig(2);
    check(glsl.find("LAND_GRID_CELLS 50") != std::string::npos && glsl.find("LAND_GRID_CHANNELS 2") != std::string::npos &&
              glsl.find("LAND_GRID_ROUND") == std::string::npos,
          "flat: its shaders' layout");
}

void round() {
    Body body;
    body.radius = kEarth;
    LandGrid g;
    g.configure(body, 0.0, 64);
    check(g.round() && g.roots() == 6 && std::fabs(g.half() - faceHalf(body)) < 1e-6, "round: six faces");
    check(g.halo().size() == 6u * (4 * 64 + 4), "round: a halo round each face");
    // Each halo cell's sources lie within a cell of it on the ground (a
    // corner's within one and a half: three cells meet there, not four).
    const int s = g.side();
    double far = 0.0, farCorner = 0.0;
    for (const LandGrid::HaloCopy& h : g.halo()) {
        const auto centre = [&](uint32_t t, double d[3]) {
            const int root = static_cast<int>(t / (s * s)), rest = static_cast<int>(t % (s * s));
            dirOf(body, {root, -g.half() + (rest % s - 0.5) * g.cell(), -g.half() + (rest / s - 0.5) * g.cell()}, d);
        };
        double a[3], b[3];
        centre(h.dst, a);
        for (const uint32_t src : h.src) {
            centre(src, b);
            const double c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            double& worst = h.src[1] == h.src[0] ? far : farCorner;
            worst = std::max(worst, kEarth * std::acos(std::min(c, 1.0)) / g.cell());
        }
    }
    check(far < 1.0, "round: a halo cell's source is within a cell of it (farthest " + std::to_string(far) + ")");
    check(farCorner < 1.5, "round: a corner's sources within 1.5 cells (farthest " + std::to_string(farCorner) + ")");
    // A smooth field of the direction, sampled anywhere on the body.
    const auto field = [](const double d[3]) { return 300.0 * d[0] + 120.0 * d[2] - 80.0 * d[1] * d[1]; };
    const std::vector<float> v = fill(g, [&](int r, double x, double z) {
        double d[3];
        dirOf(body, {r, x, z}, d);
        return field(d);
    });
    double worst = 0.0;
    uint32_t seed = 12345;
    const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0 * 2.0 - 1.0; };
    for (int k = 0; k < 20000; ++k) {
        double d[3] = {rnd(), rnd(), rnd()};
        unit(d);
        worst = std::max(worst, std::fabs(g.sampleDir(v.data(), d) - field(d)));
    }
    check(worst < 2.0, "round: the field anywhere, to within the grid's error (worst " + std::to_string(worst) + " m)");
    // Across an edge and at a corner: two points 10 m apart either side.
    double step = 0.0;
    for (const auto& p : {std::array<double, 3>{1.0, 1.0, 0.3}, std::array<double, 3>{1.0, 1.0, 1.0},
                          std::array<double, 3>{-1.0, 0.2, 1.0}, std::array<double, 3>{0.4, -1.0, -1.0}}) {
        double a[3] = {p[0], p[1], p[2]};
        unit(a);
        for (const auto& t : {std::array<double, 3>{1, -1, 0}, std::array<double, 3>{0, 1, -1}, std::array<double, 3>{1, 0, -1}}) {
            double b[3], c[3];
            for (int i = 0; i < 3; ++i) {
                b[i] = a[i] + 1e-6 * t[i];
                c[i] = a[i] - 1e-6 * t[i];
            }
            unit(b);
            unit(c);
            step = std::max(step, static_cast<double>(std::fabs(g.sampleDir(v.data(), b) - g.sampleDir(v.data(), c))));
        }
    }
    check(step < 0.05, "round: no step across an edge or a corner (largest " + std::to_string(step) + " m)");
    check(g.glslConfig(1).find("LAND_GRID_ROUND") != std::string::npos, "round: its shaders' layout");
}

}  // namespace

int main() {
    flat();
    round();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
