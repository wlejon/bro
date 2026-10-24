#include "scene/world/land_regions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bro::scene {

namespace {

double smooth01(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/// Region `r`'s channel 0 (four floats) at texel coordinates (u, v) (texel
/// centres at + 0.5): bilinear, held inside its texels.
void texelAt(const LandRegion& r, int cells, double u, double v, double out[4]) {
    u = std::clamp(u - 0.5, 0.0, cells - 1.0);
    v = std::clamp(v - 0.5, 0.0, cells - 1.0);
    const int i = std::min(static_cast<int>(u), cells - 2), j = std::min(static_cast<int>(v), cells - 2);
    const double fx = u - i, fz = v - j;
    const float* t = r.texels.data();
    const auto at = [&](int a, int b, int c) { return static_cast<double>(t[(static_cast<size_t>(b) * cells + a) * 4 + c]); };
    for (int c = 0; c < 4; ++c) {
        const double lo = at(i, j, c) + (at(i + 1, j, c) - at(i, j, c)) * fx;
        const double hi = at(i, j + 1, c) + (at(i + 1, j + 1, c) - at(i, j + 1, c)) * fx;
        out[c] = lo + (hi - lo) * fz;
    }
}

}  // namespace

int LandRegionConfig::iterations() const {
    int n = 0;
    for (const Pass& p : passes) n += std::max(p.iterations, 0);
    return n;
}

double LandRegionConfig::cellOf(int level) const { return cell * std::pow(static_cast<double>(ratio), level); }

void LandRegionConfig::sanitize(int channels) {
    cells = std::clamp(cells, 16, kMaxCells);
    apron = std::clamp(apron, 1, cells / 4);
    levels = std::clamp(levels, 0, kMaxLevels);
    ratio = std::clamp(ratio, 2, 16);
    reach = std::clamp(reach, 0, 4);
    keep = std::clamp(keep, 1, std::max(channels, 1));
    if (!(cell > 0.0)) cell = 64.0;
}

double LandRegionLattice::originX(const LandRegionKey& k) const {
    return -half + k.i * config.stepOf(k.level) - config.apron * config.cellOf(k.level);
}
double LandRegionLattice::originZ(const LandRegionKey& k) const {
    return -half + k.j * config.stepOf(k.level) - config.apron * config.cellOf(k.level);
}

void LandRegionLattice::range(int level, int& lo, int& hi) const {
    const double step = config.stepOf(level), band = round() ? config.bandOf(level) : 0.0;
    lo = static_cast<int>(std::floor(-band / step));
    hi = static_cast<int>(std::floor((2.0 * half + band) / step));
    lo = std::max(lo, -1024);
    hi = std::min(hi, 1023);
}

void LandRegionLattice::candidates(int level, double x, int& lo, int& hi) const {
    const double u = (x + half) / config.stepOf(level);
    const double a = static_cast<double>(config.apron) / (config.cells - 2 * config.apron);
    int rlo, rhi;
    range(level, rlo, rhi);
    lo = std::max(static_cast<int>(std::floor(u - a)), rlo);
    hi = std::min(static_cast<int>(std::floor(u + a)), rhi);
}

double LandRegionLattice::weight(int level, double o, double x) const {
    const double t = (x - o) / config.cellOf(level);
    const double w = 2.0 * config.apron;
    return smooth01(0.0, w, t) * smooth01(0.0, w, config.cells - t);
}

int LandRegionLattice::faces(int level, const double dir[3], int root[3], double w[3], double chart[3][2]) const {
    Body body;
    body.radius = radius;
    const double b = config.bandOf(level);
    int n = 0;
    double sum = 0.0;
    for (int f = 0; f < 6 && n < 3; ++f) {
        const CubeFace& F = cubeFace(f);
        if (dir[0] * F.n[0] + dir[1] * F.n[1] + dir[2] * F.n[2] < 0.2) continue;
        const ChartPoint c = chartOf(body, dir, f);
        const double m = std::max(std::fabs(c.x), std::fabs(c.z));
        const double wf = 1.0 - smooth01(half - b, half + b, m);
        if (wf <= 0.0) continue;
        root[n] = f;
        w[n] = wf;
        chart[n][0] = c.x;
        chart[n][1] = c.z;
        sum += wf;
        ++n;
    }
    for (int k = 0; k < n; ++k) w[k] /= sum;
    return n;
}

namespace {

/// One level of one root: its regions' weighted channel 0 at chart (x, z)
/// added to `sum` (scaled by `wf`), and their weights to `cover`.
void levelAt(const LandRegionLattice& L, const LandRegionSet& set, int level, int root, double x, double z, double wf,
             double sum[4], double& cover) {
    int ilo, ihi, jlo, jhi;
    L.candidates(level, x, ilo, ihi);
    L.candidates(level, z, jlo, jhi);
    const double cell = L.config.cellOf(level);
    for (int j = jlo; j <= jhi; ++j)
        for (int i = ilo; i <= ihi; ++i) {
            const LandRegionKey k{level, root, i, j};
            const auto it = set.find(k.packed());
            if (it == set.end()) continue;
            const double ox = L.originX(k), oz = L.originZ(k);
            const double w = wf * L.weight(level, ox, x) * L.weight(level, oz, z);
            if (w <= 0.0) continue;
            double v[4];
            texelAt(*it->second, L.config.cells, (x - ox) / cell, (z - oz) / cell, v);
            for (int c = 0; c < 4; ++c) sum[c] += w * v[c];
            cover += w;
        }
}

}  // namespace

// (Worked from the finest level out, as the GLSL is: value = Σ rest_L sum_L
// + rest coarse, rest the product of the finer levels' 1 - cover.)
void LandRegionLattice::blendDir(const LandRegionSet& set, const double dir[3], float coarse[4]) const {
    if (set.empty()) return;
    double acc[4] = {0.0, 0.0, 0.0, 0.0}, rest = 1.0;
    for (int level = 0; level < config.levels && rest > 0.0; ++level) {
        int roots[3];
        double w[3], chart[3][2];
        const int n = faces(level, dir, roots, w, chart);
        double sum[4] = {0.0, 0.0, 0.0, 0.0}, cover = 0.0;
        for (int f = 0; f < n; ++f) levelAt(*this, set, level, roots[f], chart[f][0], chart[f][1], w[f], sum, cover);
        const double p = presence(set.centre[level], dir, 0.0, 0.0);
        for (int c = 0; c < 4; ++c) acc[c] += rest * p * sum[c];
        rest *= 1.0 - p * cover;
    }
    for (int c = 0; c < 4; ++c) coarse[c] = static_cast<float>(acc[c] + std::max(rest, 0.0) * coarse[c]);
}

void LandRegionLattice::blendAt(const LandRegionSet& set, double x, double z, float coarse[4]) const {
    if (set.empty()) return;
    double acc[4] = {0.0, 0.0, 0.0, 0.0}, rest = 1.0;
    for (int level = 0; level < config.levels && rest > 0.0; ++level) {
        double sum[4] = {0.0, 0.0, 0.0, 0.0}, cover = 0.0;
        levelAt(*this, set, level, 0, x, z, 1.0, sum, cover);
        const double p = presence(set.centre[level], nullptr, x, z);
        for (int c = 0; c < 4; ++c) acc[c] += rest * p * sum[c];
        rest *= 1.0 - p * cover;
    }
    for (int c = 0; c < 4; ++c) coarse[c] = static_cast<float>(acc[c] + std::max(rest, 0.0) * coarse[c]);
}

namespace {

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

/// The angle between two unit vectors (2 asin of half the chord: exact
/// however small).
double angle3(const double a[3], const double b[3]) {
    const double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return 2.0 * std::asin(std::min(0.5 * std::sqrt(dx * dx + dy * dy + dz * dz), 1.0));
}

/// The angle from unit vector p to the great-circle arc from a to b (less
/// than a half turn).
double angleToArc(const double p[3], const double a[3], const double b[3]) {
    double n[3];
    cross3(a, b, n);
    const double l = std::sqrt(dot3(n, n));
    const double end = std::min(angle3(p, a), angle3(p, b));
    if (!(l > 0.0)) return end;
    for (double& c : n) c /= l;
    // (p's foot on the arc's circle lies between a and b: the arc's nearest
    // point is there.)
    double an[3], nb[3];
    cross3(a, p, an);
    cross3(p, b, nb);
    if (dot3(an, n) >= 0.0 && dot3(nb, n) >= 0.0) return std::min(std::asin(std::min(std::fabs(dot3(p, n)), 1.0)), end);
    return end;
}

}  // namespace

double LandRegionLattice::presence(const LandRegionCentre& c, const double dir[3], double x, double z) const {
    if (!c.active) return 1.0;
    double d;
    if (round() && dir) {
        // (Along the ground: 2 R asin of half the chord.)
        d = radius * angle3(dir, c.at.dir);
    } else {
        d = std::hypot(x - c.at.x, z - c.at.z);
    }
    if (d >= c.outer) return 0.0;
    return 1.0 - smooth01(c.inner, c.outer, d);
}

double LandRegionLattice::between(const LandGroundPoint& a, const LandGroundPoint& b) const {
    return round() ? radius * angle3(a.dir, b.dir) : std::hypot(a.x - b.x, a.z - b.z);
}

double LandRegionLattice::distanceTo(const LandGroundPoint& p, const LandRegionKey& k) const {
    const double side = config.cells * config.cellOf(k.level);
    const double x0 = originX(k), z0 = originZ(k), x1 = x0 + side, z1 = z0 + side;
    if (!round()) {
        const double dx = std::max({x0 - p.x, 0.0, p.x - x1}), dz = std::max({z0 - p.z, 0.0, p.z - z1});
        return std::hypot(dx, dz);
    }
    // A face's chart lines are great circles (x = R atan(d.u / d.n) fixed is
    // a plane through the centre), so the region's ground is a quadrilateral
    // of great-circle arcs: inside it, or the nearest of its four sides.
    Body body;
    body.radius = radius;
    if (dot3(p.dir, cubeFace(k.root).n) > 0.0) {
        const ChartPoint c = chartOf(body, p.dir, k.root);
        if (c.x >= x0 && c.x <= x1 && c.z >= z0 && c.z <= z1) return 0.0;
    }
    double q[4][3];
    dirOf(body, {k.root, x0, z0}, q[0]);
    dirOf(body, {k.root, x1, z0}, q[1]);
    dirOf(body, {k.root, x1, z1}, q[2]);
    dirOf(body, {k.root, x0, z1}, q[3]);
    double a = 4.0;
    for (int s = 0; s < 4; ++s) a = std::min(a, angleToArc(p.dir, q[s], q[(s + 1) & 3]));
    return radius * a;
}

double LandRegionLattice::discRadius(int level) const {
    return std::max(static_cast<double>(config.reach), 0.5) * config.stepOf(level);
}

bool LandRegionLattice::levelWanted(int level, double eyeHeight, double focalPixels) const {
    return config.cellOf(level) * focalPixels >= eyeHeight;
}

void LandRegionLattice::disc(int level, const LandGroundPoint& p, double r,
                             std::vector<std::pair<LandRegionKey, double>>& out) const {
    out.clear();
    if (!config.active() || !(half > 0.0)) return;
    Body body;
    body.radius = radius;
    const double step = config.stepOf(level);
    // (A face metre is a metre of ground to within a factor of two: the
    // chart window holds every region near enough.)
    const double window = 2.0 * r + config.cells * config.cellOf(level);
    int lo, hi;
    range(level, lo, hi);
    for (int f = 0; f < roots(); ++f) {
        double px = p.x, pz = p.z;
        if (round()) {
            // (The faces the reads blend at p: faces() above.)
            if (dot3(p.dir, cubeFace(f).n) < 0.2) continue;
            const ChartPoint c = chartOf(body, p.dir, f);
            px = c.x;
            pz = c.z;
        }
        const int i0 = std::max(static_cast<int>(std::floor((px - window + half) / step)) - 1, lo);
        const int i1 = std::min(static_cast<int>(std::floor((px + window + half) / step)) + 1, hi);
        const int j0 = std::max(static_cast<int>(std::floor((pz - window + half) / step)) - 1, lo);
        const int j1 = std::min(static_cast<int>(std::floor((pz + window + half) / step)) + 1, hi);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const LandRegionKey k{level, f, i, j};
                const double d = distanceTo(p, k);
                if (d < r) out.emplace_back(k, d);
            }
    }
}

void LandRegionFade::aim(const LandRegionLattice& L, int level, const LandGroundPoint& eye) {
    if (!seen) {
        seen = true;
        target = at = eye;
        radius = 0.0;
        return;
    }
    if (L.between(target, eye) > kFollow * L.discRadius(level)) target = eye;
}

void LandRegionFade::ease(const LandRegionLattice& L, double D) {
    if (at == target) return;
    for (int i = 0; i < 3; ++i) at.dir[i] += (target.dir[i] - at.dir[i]) * kEase;
    const double l = std::sqrt(dot3(at.dir, at.dir));
    if (l > 0.0)
        for (double& c : at.dir) c /= l;
    at.x += (target.x - at.x) * kEase;
    at.z += (target.z - at.z) * kEase;
    if (L.between(at, target) < kSnap * D) at = target;
}

LandRegionCentre LandRegionFade::centre() const {
    LandRegionCentre c;
    c.active = seen;
    c.at = at;
    c.inner = kInner * radius;
    c.outer = kOuter * radius;
    return c;
}

void LandRegionStore::add(std::shared_ptr<const LandRegion> region) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = std::make_shared<LandRegionSet>(*set_);
    (*next)[region->key.packed()] = std::move(region);
    set_ = std::move(next);
}

void LandRegionStore::remove(const LandRegionKey& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!set_->count(key.packed())) return;
    auto next = std::make_shared<LandRegionSet>(*set_);
    next->erase(key.packed());
    set_ = std::move(next);
}

void LandRegionStore::setCentres(const LandRegionCentre* centres) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto next = std::make_shared<LandRegionSet>(*set_);
    for (int l = 0; l < LandRegionConfig::kMaxLevels; ++l) next->centre[l] = centres[l];
    set_ = std::move(next);
}

void LandRegionStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    set_ = std::make_shared<const LandRegionSet>();
}

void landRegionsWanted(const LandRegionLattice& L, const double eyeDir[3], double flatX, double flatZ,
                       double eyeHeight, double focalPixels, std::vector<LandRegionKey>& wanted,
                       std::vector<LandRegionKey>* near, const LandGroundPoint* centres) {
    wanted.clear();
    if (near) near->clear();
    if (!L.config.active() || !(L.half > 0.0)) return;
    Body body;
    body.radius = L.radius;
    LandGroundPoint eye;
    if (L.round())
        for (int i = 0; i < 3; ++i) eye.dir[i] = eyeDir[i];
    eye.x = flatX;
    eye.z = flatZ;
    struct Ranked {
        LandRegionKey key;
        double d, mid;
    };
    std::vector<std::pair<LandRegionKey, double>> keys;
    std::vector<Ranked> level;
    for (int lv = L.config.levels - 1; lv >= 0; --lv) {
        // (A level whose cell is under a pixel from the eye's height is not wanted.)
        if (!L.levelWanted(lv, eyeHeight, focalPixels)) continue;
        const LandGroundPoint& c = centres ? centres[lv] : eye;
        L.disc(lv, c, L.discRadius(lv), keys);
        level.clear();
        const double step = L.config.stepOf(lv);
        for (const auto& [k, d] : keys) {
            // (Nearest first: by the ground's distance, then the region's middle's.)
            LandGroundPoint m;
            m.x = -L.half + (k.i + 0.5) * step;
            m.z = -L.half + (k.j + 0.5) * step;
            if (L.round()) dirOf(body, {k.root, m.x, m.z}, m.dir);
            level.push_back({k, d, L.between(c, m)});
            if (!near) continue;
            // (Holding the eye's ground: its weight there is not zero.)
            double ex = flatX, ez = flatZ;
            if (L.round()) {
                if (dot3(eyeDir, cubeFace(k.root).n) <= 0.0) continue;
                const ChartPoint e = chartOf(body, eyeDir, k.root);
                ex = e.x;
                ez = e.z;
            }
            if (L.weight(lv, L.originX(k), ex) > 0.0 && L.weight(lv, L.originZ(k), ez) > 0.0) near->push_back(k);
        }
        std::sort(level.begin(), level.end(),
                  [](const Ranked& a, const Ranked& b) { return a.d < b.d || (a.d == b.d && a.mid < b.mid); });
        for (const Ranked& r : level) wanted.push_back(r.key);
    }
}

int LandRegionLattice::slots() const {
    const int n = (2 * config.reach + 1) * (2 * config.reach + 1);
    return config.levels * n * 2 + n;
}

int LandRegionLattice::tableSize() const {
    int t = 64;
    while (t < 4 * slots()) t *= 2;
    return t;
}

uint32_t LandRegionLattice::hash(uint32_t packed, int tableSize) {
    int bits = 0;
    while ((1 << bits) < tableSize) ++bits;
    return (packed * 2654435761u) >> (32 - bits);
}

std::string LandRegionLattice::glslConfig() const {
    if (!config.active()) return std::string();
    int bits = 0;
    while ((1 << bits) < tableSize()) ++bits;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "#define LAND_REGIONS 1\n"
                  "#define LAND_REGION_LEVELS %d\n"
                  "#define LAND_REGION_CELLS %d\n"
                  "#define LAND_REGION_APRON %d\n"
                  "#define LAND_REGION_KEEP %d\n"
                  "#define LAND_REGION_TABLE %d\n"
                  "#define LAND_REGION_TABLE_BITS %d\n"
                  "const float LAND_REGION_CELL0 = %.9g;\n"
                  "const float LAND_REGION_RATIO = %d.0;\n",
                  config.levels, config.cells, config.apron, config.keep, tableSize(), bits, config.cell,
                  config.ratio);
    return buf;
}

bool landRegionsOverlap(const LandRegionLattice& L, const LandRegionKey& a, const LandRegionKey& b) {
    if (a.root != b.root) return false;
    const double sa = L.config.cells * L.config.cellOf(a.level), sb = L.config.cells * L.config.cellOf(b.level);
    const double ax = L.originX(a), az = L.originZ(a), bx = L.originX(b), bz = L.originZ(b);
    return ax < bx + sb && bx < ax + sa && az < bz + sb && bz < az + sa;
}

}  // namespace bro::scene
