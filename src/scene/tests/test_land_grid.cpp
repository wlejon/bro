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

// --- regional grids (scene/world/land_regions.h) ---------------------------

// Every region of `levels` levels on every root of `L`, each texel channel 0
// `f(dir or chart point, level)` at its centre.
template <class F>
LandRegionSet everyRegion(const LandRegionLattice& L, const Body& body, F f) {
    LandRegionSet set;
    const int n = L.config.cells;
    for (int level = 0; level < L.config.levels; ++level) {
        int lo, hi;
        L.range(level, lo, hi);
        const double cell = L.config.cellOf(level);
        for (int root = 0; root < L.roots(); ++root)
            for (int j = lo; j <= hi; ++j)
                for (int i = lo; i <= hi; ++i) {
                    auto r = std::make_shared<LandRegion>();
                    r->key = {level, root, i, j};
                    r->texels.assign(static_cast<size_t>(n) * n * 4, 0.0f);
                    const double ox = L.originX(r->key), oz = L.originZ(r->key);
                    for (int b = 0; b < n; ++b)
                        for (int a = 0; a < n; ++a) {
                            const double x = ox + (a + 0.5) * cell, z = oz + (b + 0.5) * cell;
                            double d[3] = {x, 0.0, z};
                            if (L.round()) dirOf(body, {root, x, z}, d);
                            r->texels[(static_cast<size_t>(b) * n + a) * 4] = static_cast<float>(f(d, level));
                        }
                    set[r->key.packed()] = std::move(r);
                }
    }
    return set;
}

void regions() {
    // Flat: one level's regions, each a constant, cover the root whole (their
    // weights a partition of one), so the global grid's value is not read
    // inside them, and the blend is continuous across the regions' aprons.
    LandRegionLattice F;
    F.config.init = "init";
    F.config.cells = 64;
    F.config.apron = 8;
    F.config.cell = 10.0;
    F.config.levels = 1;
    F.half = 1000.0;
    Body flatBody;
    const LandRegionSet constant = everyRegion(F, flatBody, [](const double*, int) { return 7.0; });
    double worst = 0.0;
    // (Inside the root less an apron: at a flat root's own edge the
    // outermost regions fade into the global grid.)
    for (double x = -915.0; x < 915.0; x += 7.3)
        for (double z = -915.0; z < 915.0; z += 11.1) {
            float c[4] = {3.0f, 0.0f, 0.0f, 0.0f};
            F.blendAt(constant, x, z, c);
            worst = std::max(worst, std::fabs(c[0] - 7.0));
        }
    check(worst < 1e-4, "flat regions: their weights sum to one over the root (worst " + std::to_string(worst) + ")");
    // A smooth field sampled into the regions comes back to within its
    // bilinear error, with no step where the regions overlap.
    const auto smooth = [](const double* p, int) { return 40.0 * std::sin(p[0] / 120.0) * std::cos(p[2] / 170.0); };
    const LandRegionSet field = everyRegion(F, flatBody, smooth);
    double err = 0.0, step = 0.0;
    for (double x = -900.0; x < 900.0; x += 3.7) {
        const double z = 0.37 * x;
        float a[4] = {0}, b[4] = {0};
        F.blendAt(field, x, z, a);
        F.blendAt(field, x + 1e-3, z, b);
        const double p[3] = {x, 0.0, z};
        err = std::max(err, std::fabs(a[0] - smooth(p, 0)));
        step = std::max(step, static_cast<double>(std::fabs(a[0] - b[0])));
    }
    check(err < 0.1, "flat regions: a smooth field to within its bilinear error (" + std::to_string(err) + " m)");
    check(step < 0.01, "flat regions: no step across their aprons (" + std::to_string(step) + " m)");
    // With none in, the coarser field (the global grid's) is read as it is.
    float c[4] = {3.0f, 1.0f, 2.0f, 4.0f};
    F.blendAt({}, 10.0, 20.0, c);
    check(c[0] == 3.0f, "flat regions: none in, the global grid's");

    // Round: a small body's six faces at two levels, the field a function of
    // the direction: read back across faces' edges and corners with no step,
    // the finer level over the coarser one.
    Body body;
    body.radius = 20000.0;
    LandRegionLattice R;
    R.config.init = "init";
    R.config.cells = 64;
    R.config.apron = 8;
    R.config.cell = 25.0;
    R.config.ratio = 4;
    R.config.levels = 2;
    R.half = faceHalf(body);
    R.radius = body.radius;
    const auto dirField = [&](const double* d, int) { return 300.0 * d[0] * d[1] + 200.0 * d[2] * d[2]; };
    const LandRegionSet roundSet = everyRegion(R, body, dirField);
    double rerr = 0.0, rstep = 0.0;
    for (int k = 0; k < 4000; ++k) {
        const double t = k * 0.0123, s = k * 0.00731;
        double d[3] = {std::cos(t) * std::cos(s), std::sin(s), std::sin(t) * std::cos(s)};
        unit(d);
        float a[4] = {-1e6f, 0, 0, 0};
        R.blendDir(roundSet, d, a);
        rerr = std::max(rerr, std::fabs(a[0] - dirField(d, 0)));
        double e[3] = {d[0] + 2e-9, d[1] - 1e-9, d[2] + 1e-9};
        unit(e);
        float b[4] = {-1e6f, 0, 0, 0};
        R.blendDir(roundSet, e, b);
        rstep = std::max(rstep, static_cast<double>(std::fabs(a[0] - b[0])));
    }
    check(rerr < 0.05, "round regions: the field over every face, edge and corner (" + std::to_string(rerr) + " m)");
    check(rstep < 0.01, "round regions: no step anywhere (" + std::to_string(rstep) + " m)");
    // The finer level is what is read where it is in: level 0 a different
    // value than level 1 reads level 0's.
    const LandRegionSet byLevel = everyRegion(R, body, [](const double*, int level) { return level == 0 ? 5.0 : 9.0; });
    double lworst = 0.0;
    for (int k = 0; k < 500; ++k) {
        double d[3] = {std::cos(k * 0.1), 0.3 * std::sin(k * 0.37), std::sin(k * 0.1)};
        unit(d);
        float a[4] = {1.0f, 0, 0, 0};
        R.blendDir(byLevel, d, a);
        lworst = std::max(lworst, std::fabs(a[0] - 5.0));
    }
    check(lworst < 1e-4, "round regions: the finest level in covers the coarser (" + std::to_string(lworst) + ")");
    check(R.glslConfig().find("LAND_REGION") != std::string::npos, "round regions: their shaders' layout");
}

// PRESENCE: a level is read in a disc about the eye that what is in covers
// whole, its fade ending inside it with a region still to come, round (no
// region's straight edge), and growing over frames as regions arrive.
void presence() {
    LandRegionLattice F;
    F.config.init = "init";
    F.config.cells = 64;
    F.config.apron = 8;
    F.config.cell = 10.0;
    F.config.levels = 1;
    F.config.reach = 1;
    F.half = 1000.0;
    Body flatBody;
    LandRegionSet set = everyRegion(F, flatBody, [](const double*, int) { return 7.0; });
    const double D = F.discRadius(0);
    check(D == 480.0, "presence: the disc a lattice step at reach 1");
    // The eye at the root's middle; region (0, 2), its ground from 440 m
    // west, is in the disc and not in.
    LandGroundPoint eye;
    const LandRegionKey gone{0, 0, 0, 2};
    set.erase(gone.packed());
    check(std::fabs(F.distanceTo(eye, gone) - 440.0) < 1e-9, "presence: a flat region's distance");
    std::vector<std::pair<LandRegionKey, double>> keys;
    F.disc(0, eye, D, keys);
    bool listed = false;
    for (const auto& [k, d] : keys) listed = listed || k == gone;
    check(listed, "presence: the missing region is in the wanted disc");
    const auto in = [&](const LandRegionKey& k) { return set.count(k.packed()) > 0; };
    const double covered = F.covered(0, eye, eye, in);
    check(std::fabs(covered - 440.0) < 1e-9, "presence: covered to the missing region (" + std::to_string(covered) + ")");
    LandRegionFade fade;
    fade.aim(F, 0, eye);
    LandRegionCentre c = fade.step(F, 0, in);
    check(c.active && c.outer > 0.0 && c.outer < 0.5 * covered, "presence: the disc grows, no jump (first outer " +
                                                                    std::to_string(c.outer) + ")");
    for (int f = 0; f < 60; ++f) {
        fade.aim(F, 0, eye);
        c = fade.step(F, 0, in);
        check(c.outer <= covered + 1e-9, "presence: the fade ends inside what is in, every frame");
    }
    check(c.outer > 0.85 * covered, "presence: the disc grown to what is in (" + std::to_string(c.outer) + ")");
    // Read with it: the region's level whole inside, the coarser field from
    // the fade's end on, all round (toward the missing region and away).
    set.centre[0] = c;
    double insideWorst = 0.0, outsideWorst = 0.0;
    for (int a = 0; a < 64; ++a) {
        const double t = a * 0.0981747704;   // (2 pi / 64)
        float v[4] = {3.0f, 0, 0, 0}, w[4] = {3.0f, 0, 0, 0};
        F.blendAt(set, 0.95 * c.inner * std::cos(t), 0.95 * c.inner * std::sin(t), v);
        F.blendAt(set, 1.01 * c.outer * std::cos(t), 1.01 * c.outer * std::sin(t), w);
        insideWorst = std::max(insideWorst, std::fabs(v[0] - 7.0));
        outsideWorst = std::max(outsideWorst, std::fabs(w[0] - 3.0));
    }
    check(insideWorst < 1e-4, "presence: the level whole inside its fade (" + std::to_string(insideWorst) + ")");
    check(outsideWorst < 1e-6, "presence: the coarser field past it, round (" + std::to_string(outsideWorst) + ")");
    // The region in: the disc grows to the wanted disc's edge over frames.
    set = everyRegion(F, flatBody, [](const double*, int) { return 7.0; });
    const double before = c.outer;
    c = fade.step(F, 0, in);
    check(c.outer > before && c.outer < LandRegionFade::kOuter * D - 1.0, "presence: the region in, the disc eased out");
    for (int f = 0; f < 60; ++f) c = fade.step(F, 0, in);
    check(std::fabs(c.outer - LandRegionFade::kOuter * D) < 1e-6, "presence: then to the wanted disc's");
    // The eye moving: the centre follows it, eased, the fade inside what is in.
    for (int f = 0; f < 200; ++f) {
        eye.x = 2.0 * f;
        fade.aim(F, 0, eye);
        c = fade.step(F, 0, in);
        check(c.outer <= F.covered(0, c.at, fade.target, in) + 1e-9, "presence: inside what is in as the eye moves");
    }
    for (int f = 0; f < 60; ++f) c = fade.step(F, 0, in);
    check(c.at == fade.target && F.between(c.at, eye) <= LandRegionFade::kFollow * D,
          "presence: the centre followed the eye (" + std::to_string(F.between(c.at, eye)) + " m)");

    // Round: an eye 500 km from a cube face's edge on an Earth; the wanted
    // disc crosses onto the next face, and a region's distance is its
    // ground's nearest (against a dense search of its footprint).
    Body body;
    body.radius = kEarth;
    LandRegionLattice R;
    R.config.init = "init";
    R.config.cells = 512;
    R.config.apron = 32;
    R.config.cell = 4096.0;
    R.config.levels = 1;
    R.half = faceHalf(body);
    R.radius = body.radius;
    LandGroundPoint p;
    dirOf(body, {2, R.half - 500000.0, 1234567.0}, p.dir);
    R.disc(0, p, R.discRadius(0), keys);
    int roots[6] = {0, 0, 0, 0, 0, 0};
    double worst = 0.0;
    for (const auto& [k, d] : keys) {
        ++roots[k.root];
        // (The footprint's ground, densely: the nearest of it.)
        const double side = R.config.cells * R.config.cellOf(0);
        double best = 1e300;
        for (int b = 0; b <= 200; ++b)
            for (int a = 0; a <= 200; ++a) {
                double q[3];
                dirOf(body, {k.root, R.originX(k) + side * a / 200.0, R.originZ(k) + side * b / 200.0}, q);
                const double dx = q[0] - p.dir[0], dy = q[1] - p.dir[1], dz = q[2] - p.dir[2];
                best = std::min(best, 2.0 * kEarth * std::asin(std::min(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz), 1.0)));
            }
        check(d <= best + 1.0, "presence: a region's distance never more than its ground's");
        worst = std::max(worst, best - d);
    }
    int faces = 0;
    for (int n : roots) faces += n > 0;
    check(faces == 2, "presence: the disc 500 km from an edge on two faces (" + std::to_string(faces) + ")");
    check(worst < 20000.0, "presence: a region's distance its ground's (worst short by " + std::to_string(worst) + " m)");
}

}  // namespace

int main() {
    flat();
    round();
    regions();
    presence();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
