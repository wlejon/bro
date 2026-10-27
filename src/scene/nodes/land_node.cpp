#include "scene/nodes/land_node.h"

#include "scene/nodes/world_module.h"
#include "scene/world/land_sun.h"
#include "scene/world/world_workers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace bro::scene {

namespace {

// The land's height range is sampled in cells of about this many metres (at
// most kItemCells a side over the extent), each cell's samples this far
// apart, and widened by kItemMargin of its range plus a metre: a tile's box
// (its footprint on screen and its frustum test) only needs to hold the
// ground, not to be tight.
constexpr float kItemMetres = 32.0f;
constexpr int kItemCells = 256;
constexpr int kItemSamples = 5;   // a side, a cell
constexpr float kItemMargin = 0.15f;
// A round body's: kFaceCells a side a cube face.
constexpr int kFaceCells = 128;

double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

void normalize3(double v[3]) {
    const double l = std::sqrt(dot3(v, v));
    for (int i = 0; i < 3; ++i) v[i] /= l;
}

/// The y of the ground of height `h` under place (x, z) on a body of radius
/// `R` whose place origin is on its datum: |(x, R + y, z)| = R + h, written
/// so that it does not take R from R.
double groundY(double R, double h, double x, double z) {
    const double rho2 = x * x + z * z;
    const double a = (R + h) * (R + h) - rho2;
    if (!(a >= 0.0)) return std::numeric_limits<double>::quiet_NaN();
    return (h * (2.0 * R + h) - rho2) / (std::sqrt(a) + R);
}

}  // namespace

LandNode::LandNode(const std::string& name) : MeshNode(name) {
    setColor(1.0f, 1.0f, 1.0f, 1.0f);
    setMetallic(0.0f);
    setRoughness(0.95f);
    setCastsShadow(true);
    setReceivesShadow(true);
    buildRings();
    rebuildItems();
}

LandNode::~LandNode() = default;

bool LandNode::setModule(std::string source, const float* data, size_t floats, const std::string& heightTree,
                         int seed, std::string& err) {
    FieldRef height;
    if (!heightTree.empty()) {
        height = Field::make(heightTree, seed);
        if (!height) {
            err = "the height's node tree does not decode";
            return false;
        }
    }
    source_ = std::move(source);
    sourceHash_ = scene::moduleSourceHash(source_);
    floats = std::min(floats, kMaxData * 4);
    data_.assign(data, data + floats);
    data_.resize((floats + 3) / 4 * 4, 0.0f);
    height_ = std::move(height);
    ++generation_;
    rebuildItems();
    return true;
}

bool LandNode::setProcess(LandProcess process, std::string& err) {
    const auto identifier = [](const std::string& s) {
        if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
        return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    };
    for (const LandProcess::Pass& p : process.passes)
        if (!identifier(p.fn)) {
            err = "a pass's function '" + p.fn + "' is not a GLSL name";
            return false;
        }
    if (!process.regions.init.empty() && !identifier(process.regions.init)) {
        err = "the regions' init '" + process.regions.init + "' is not a GLSL name";
        return false;
    }
    for (const LandRegionConfig::Pass& p : process.regions.passes)
        if (!identifier(p.fn)) {
            err = "a region pass's function '" + p.fn + "' is not a GLSL name";
            return false;
        }
    process.channels = std::clamp(process.channels, 1, LandProcess::kMaxChannels);
    process.regions.sanitize(process.channels);
    process.resolution = std::clamp(process.resolution, 1, LandProcess::kMaxResolution);
    process.data.resize(std::min(process.data.size(), LandProcess::kMaxData * 4));
    process_ = std::move(process);
    configureGrid();
    ++generation_;
    rebuildItems();
    return true;
}

void LandNode::configureGrid() {
    // Run again over the land as it is now; until then not ready.
    if (process_.active()) grid_.configure(round() ? place_->body() : Body{}, extent_, process_.resolution);
    ++gridSerial_;
    base_.reset();
    // Its regions over the grid as it is now, none of them in.
    lattice_ = {};
    if (process_.active() && process_.regions.active()) {
        lattice_.config = process_.regions;
        lattice_.half = grid_.half();
        lattice_.radius = grid_.radius();
    }
    regionStore_ = std::make_shared<LandRegionStore>();
    regionsSettled_ = true;
}

std::string LandNode::regionDispatch() const {
    const LandRegionConfig& r = process_.regions;
    std::string s = "void procRegionInit(inout vec4 v[PROC_CHANNELS]) { " + r.init + "(v); }\n";
    s += "void procRegionRun(int pass, inout vec4 v[PROC_CHANNELS]) {\n    switch (pass) {\n";
    for (size_t i = 0; i < r.passes.size(); ++i)
        s += "    case " + std::to_string(i) + ": " + r.passes[i].fn + "(v); break;\n";
    s += "    default: break;\n    }\n}\n";
    return s;
}

void LandNode::regionChanged(const LandRegionKey& key, const LandRegionCentre& centre) {
    if (!lattice_.config.active() || items_.empty() || itemCells_ <= 0) return;
    const double cell = lattice_.config.cellOf(key.level);
    const double side = lattice_.config.cells * cell;
    const double x0 = lattice_.originX(key), z0 = lattice_.originZ(key);
    const double half = round() ? faceHalf(place_->body()) : static_cast<double>(extent_);
    const double item = 2.0 * half / itemCells_;
    const uint64_t g = generation_ ^ (++itemBumps_ * 0x9e3779b97f4a7c15ull);
    // Where the level is read: past its centre's outer radius (with a
    // margin for a tile's border and normals) no texel reads the region, so
    // its items there keep their tiles (when the disc grows over them, its
    // fade stales them, regionFadeChanged). Compared as chords on a round
    // body. (Each region streamed in at a disc's edge re-filled the hundred
    // and more tiles over it, though none of them read it yet.)
    const double reach = centre.outer + 2.0 * item;
    const double R = round() ? place_->body().radius : 0.0;
    const double chordReach = round() ? 2.0 * std::sin(std::min(reach / (2.0 * R), 1.5707963267948966)) : reach;
    const auto read = [&](const double* d, double x, double z) {
        if (!centre.active) return true;
        if (round()) {
            const double a = d[0] - centre.at.dir[0], b = d[1] - centre.at.dir[1], c = d[2] - centre.at.dir[2];
            return a * a + b * b + c * c <= chordReach * chordReach;
        }
        return std::hypot(x - centre.at.x, z - centre.at.z) <= reach;
    };
    // The items under the region, a margin of one item round it (a tile's
    // border and its normals read past its edge): its ground sampled every
    // half item, each sample's item moved on (on a round body the sample's own
    // face's, so a region reaching over an edge stales the next face's too).
    const int steps = static_cast<int>(std::ceil((side + 2.0 * item) / (0.5 * item))) + 1;
    std::vector<size_t> touched;
    for (int b = 0; b < steps; ++b)
        for (int a = 0; a < steps; ++a) {
            const double x = x0 - item + a * 0.5 * item, z = z0 - item + b * 0.5 * item;
            int root = 0;
            double cx = x, cz = z;
            double d[3] = {0.0, 0.0, 0.0};
            if (round()) dirOf(place_->body(), {key.root, x, z}, d);
            if (!read(d, x, z)) continue;
            if (round()) {
                const ChartPoint p = chartOf(place_->body(), d);
                root = p.root;
                cx = p.x;
                cz = p.z;
            }
            const int i = static_cast<int>(std::floor((cx + half) / item));
            const int j = static_cast<int>(std::floor((cz + half) / item));
            if (i < 0 || j < 0 || i >= itemCells_ || j >= itemCells_) continue;
            const size_t k = (static_cast<size_t>(root) * itemCells_ + j) * itemCells_ + i;
            if (k < items_.size()) touched.push_back(k);
        }
    std::sort(touched.begin(), touched.end());
    touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
    refreshItems(std::move(touched), g);
}

void LandNode::regionFadeChanged(const LandRegionCentre& before, const LandRegionCentre& after) {
    if (!lattice_.config.active() || items_.empty() || itemCells_ <= 0) return;
    const bool sphere = round();
    const int roots = sphere ? 6 : 1;
    const double half = sphere ? faceHalf(place_->body()) : static_cast<double>(extent_);
    const double item = 2.0 * half / itemCells_;
    const size_t n = static_cast<size_t>(roots) * itemCells_ * itemCells_;
    if (n > items_.size()) return;
    // Each item's middle (a body direction, or its chart point), kept.
    if (itemMids_.size() != n * 3 || itemMidsHalf_ != half) {
        itemMids_.assign(n * 3, 0.0);
        itemMidsHalf_ = half;
        for (size_t k = 0; k < n; ++k) {
            const int root = static_cast<int>(k / (static_cast<size_t>(itemCells_) * itemCells_));
            const int j = static_cast<int>(k / itemCells_ % itemCells_), i = static_cast<int>(k % itemCells_);
            const double x = -half + (i + 0.5) * item, z = -half + (j + 0.5) * item;
            double* m = &itemMids_[k * 3];
            if (sphere) {
                dirOf(place_->body(), {root, x, z}, m);
            } else {
                m[0] = x;
                m[2] = z;
            }
        }
    }
    // Where a level's centre reads it all, none of it, or some: an item
    // (with a margin for its tile's border and normals) is made again unless
    // it is all or none of it both before and after. Compared as chords on a
    // round body.
    const double margin = 2.0 * item, R = sphere ? place_->body().radius : 0.0;
    const auto chord = [&](double d) {
        if (!sphere) return d;
        return d <= 0.0 ? -1.0 : 2.0 * std::sin(std::min(d / (2.0 * R), 1.5707963267948966));
    };
    struct Bounds {
        bool active;
        double c[3];
        double whole, none;   // within whole: all of it; past none: none of it
    };
    const auto boundsOf = [&](const LandRegionCentre& c) {
        Bounds b{c.active, {0.0, 0.0, 0.0}, chord(c.inner - margin), chord(c.outer + margin)};
        if (sphere) {
            for (int i = 0; i < 3; ++i) b.c[i] = c.at.dir[i];
        } else {
            b.c[0] = c.at.x;
            b.c[2] = c.at.z;
        }
        return b;
    };
    const Bounds a = boundsOf(before), b = boundsOf(after);
    // 1 all of it, 0 none, -1 some.
    const auto read = [](const Bounds& c, const double* m) {
        if (!c.active) return 1;
        const double dx = m[0] - c.c[0], dy = m[1] - c.c[1], dz = m[2] - c.c[2];
        const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d <= c.whole) return 1;
        if (d >= c.none) return 0;
        return -1;
    };
    const uint64_t g = generation_ ^ (++itemBumps_ * 0x9e3779b97f4a7c15ull);
    std::vector<size_t> ks;
    for (size_t k = 0; k < n; ++k) {
        const double* m = &itemMids_[k * 3];
        const int ra = read(a, m), rb = read(b, m);
        if (ra == rb && ra >= 0) continue;
        ks.push_back(k);
    }
    refreshItems(std::move(ks), g);
}

std::string LandNode::gridConfig() const { return process_.active() ? grid_.glslConfig(process_.channels) : std::string(); }

std::string LandNode::processDispatch() const {
    std::string s = "void procRun(int pass, inout vec4 v[PROC_CHANNELS]) {\n    switch (pass) {\n";
    for (size_t i = 0; i < process_.passes.size(); ++i)
        s += "    case " + std::to_string(i) + ": " + process_.passes[i].fn + "(v); break;\n";
    s += "    default: break;\n    }\n}\n";
    return s;
}

void LandNode::setGridBase(uint64_t serial, std::vector<float> channel0) {
    const size_t n = grid_.texels();
    if (serial != gridSerial_ || channel0.size() != n * 4) return;
    LandBase b{grid_, {}, {}, {}};
    b.values.resize(n);
    b.scales.resize(n);
    b.own[0].resize(n);
    b.own[1].resize(n);
    for (size_t i = 0; i < n; ++i) {
        b.values[i] = channel0[i * 4];
        b.scales[i] = channel0[i * 4 + 1];
        b.own[0][i] = channel0[i * 4 + 2];
        b.own[1][i] = channel0[i * 4 + 3];
    }
    b.lattice = lattice_;
    b.regions = regionStore_;
    base_ = std::make_shared<const LandBase>(std::move(b));
    baseSerial_ = serial;
    // Its heights (and so its tiles' boxes) are the tree's over the base:
    // the items made again on a worker, a new generation once they are in
    // (the whole body's boxes are seconds of heights on one thread).
    rebuildItems(/*now=*/false);
    baseItems_ = itemsAsked_;
}

float LandNode::overTree(float tree, int root, double x, double z) const {
    return base_ ? base_->over(tree, root, x, z) : tree;
}

void LandNode::setCanopy(std::string source, float top, float range, float castRange) {
    const uint64_t hash = source.empty() ? 0 : scene::moduleSourceHash(source);
    top = source.empty() ? 0.0f : std::max(top, 0.0f);
    // (The ranges are the draw's alone: the tiles stay.)
    canopyRange_ = range;
    canopyCastRange_ = castRange;
    if (hash == canopyHash_ && top == canopyTop_) return;
    canopy_ = std::move(source);
    canopyHash_ = hash;
    canopyTop_ = top;
    ++generation_;
    rebuildItems();
}

void LandNode::setExtent(float extent) {
    if (!(extent > 0.0f)) return;
    extent_ = extent;
    configureGrid();
    ++generation_;
    rebuildItems();
}

void LandNode::setPlace(const Place* place) {
    place_ = place;
    map_.set(place_, lowest());
    // (A re-anchor moves the place, not the land: only a new body is new.)
    const double radius = round() ? place_->body().radius : 0.0;
    if (radius == radius_) return;
    radius_ = radius;
    configureGrid();
    ++generation_;
    buildRings();
    rebuildItems();
}

double LandNode::mapReach() const {
    return round() ? 3.14159265358979323846 * place_->body().radius : static_cast<double>(extent_);
}

bromath::AABB3 LandNode::bounds() const {
    if (!round()) return {chartBounds_.min, {chartBounds_.max.x, highest(), chartBounds_.max.z}};
    // The body about its centre, (0, -R, 0) in the place frame (place.h: the
    // place origin is on its datum, its y up there); the place may tilt, the
    // ball does not care.
    const double r = place_->body().radius + std::max(static_cast<double>(highest()), 0.0);
    const float R = static_cast<float>(place_->body().radius), rf = static_cast<float>(r);
    return {{-rf, -R - rf, -rf}, {rf, -R + rf, rf}};
}

void LandNode::pendingWork(std::vector<std::string>& out) const {
    if (!ready()) out.push_back("land processes: not yet run");
    if (stats_.regionsPending)
        out.push_back("land: " + std::to_string(stats_.regionsPending) + " regions pending");
    if (stats_.missing) out.push_back("land: " + std::to_string(stats_.missing) + " tiles missing");
    if (const uint64_t n = itemBatchesPending())
        out.push_back("land: " + std::to_string(n) + " batches of tile boxes being made");
}

float LandNode::heightOver(const double dir[3]) const {
    const double R = place_->body().radius;
    const float tree = height_ ? height_->at(R * dir[0], R * dir[1], R * dir[2]) : 0.0f;
    return base_ ? base_->overDir(tree, dir) : tree;
}

void LandNode::setRings(const Rings& rings) {
    Rings r = rings;
    // The rings tile exactly only when a ring's hole is a whole number of
    // cells inset (buildRings), so the resolution is a multiple of 4.
    r.resolution = std::clamp(r.resolution / 4 * 4, 8, 1024);
    r.levels = std::clamp(r.levels, 1, kMaxRings);
    if (!(r.cellSize > 0.0f)) r.cellSize = rings_.cellSize;
    rings_ = r;
    buildRings();
    ++generation_;
}

int LandNode::wantedRings() const {
    if (!round()) return rings_.levels;
    // On a round body, as many as reach round it: the coarsest ring's
    // half-side (half its cells of 2^(L-1) finest cells) at least πR.
    int L = rings_.levels;
    while (L < kMaxRings && static_cast<double>(rings_.cellSize) * std::exp2(L - 1) * 0.5 * rings_.resolution < mapReach())
        ++L;
    return L;
}

float LandNode::heightAt(float x, float z) const {
    if (!ready()) return std::numeric_limits<float>::quiet_NaN();
    if (!round()) return overTree(height_ ? height_->at(x, z) : 0.0f, 0, x, z);
    // The ground's height is the body direction's, which is the ground's
    // own: guess the datum, take the height there, and again from where
    // that puts the ground (the direction barely turns with the height).
    const double R = place_->body().radius;
    double y = 0.0;
    for (int it = 0; it < 2; ++it) {
        const double p[3] = {x, y, z};
        double d[3];
        place_->toBody(p, d);
        normalize3(d);
        y = groundY(R, heightOver(d), x, z);
    }
    return static_cast<float>(y);
}

void LandNode::sampleHeights(float x0, float z0, float cell, int cols, int rows, float* out) const {
    const size_t n = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    if (!ready()) {
        std::fill(out, out + n, std::numeric_limits<float>::quiet_NaN());
        return;
    }
    std::vector<float> xs(n), zs(n);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            xs[static_cast<size_t>(j) * cols + i] = x0 + static_cast<float>(i) * cell;
            zs[static_cast<size_t>(j) * cols + i] = z0 + static_cast<float>(j) * cell;
        }
    if (round()) {
        // heightAt's two steps, the grid's every point at once: the
        // body's points relative to the place origin.
        const double R = place_->body().radius;
        std::vector<double> y(n, 0.0);
        std::vector<float> bx(n), by(n), bz(n);
        const double* o = place_->origin();
        for (int it = 0; it < 2; ++it) {
            std::vector<double> dirs(n * 3);
            for (size_t k = 0; k < n; ++k) {
                const double p[3] = {xs[k], y[k], zs[k]};
                double* d = &dirs[k * 3];
                place_->toBody(p, d);
                normalize3(d);
                bx[k] = static_cast<float>(R * d[0] - o[0]);
                by[k] = static_cast<float>(R * d[1] - o[1]);
                bz[k] = static_cast<float>(R * d[2] - o[2]);
            }
            if (height_)
                height_->sample(bx.data(), by.data(), bz.data(), static_cast<int>(n), o, out);
            else
                std::fill(out, out + n, 0.0f);
            if (base_)
                for (size_t k = 0; k < n; ++k) out[k] = base_->overDir(out[k], &dirs[k * 3]);
            for (size_t k = 0; k < n; ++k) y[k] = groundY(R, out[k], xs[k], zs[k]);
        }
        for (size_t k = 0; k < n; ++k) out[k] = static_cast<float>(y[k]);
        return;
    }
    if (height_) {
        height_->sample(xs.data(), zs.data(), static_cast<int>(n), out);
    } else {
        std::fill(out, out + n, 0.0f);
    }
    for (size_t k = 0; k < n; ++k) {
        out[k] = overTree(out[k], 0, xs[k], zs[k]);
        if (std::fabs(xs[k]) > extent_ || std::fabs(zs[k]) > extent_) out[k] = std::numeric_limits<float>::quiet_NaN();
    }
}

bool LandNode::raycast(const bromath::Vec3& origin, const bromath::Vec3& dir, float maxDist, float& dist,
                       bromath::Vec3& normal) const {
    // The span of the ray over the land's box.
    float t0 = 0.0f, t1 = maxDist;
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
    const bromath::AABB3 box = bounds();
    const float lo[3] = {box.min.x, box.min.y, box.min.z}, hi[3] = {box.max.x, box.max.y, box.max.z};
    if (lo[0] > hi[0]) return false;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(d[k]) < 1e-12f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
            continue;
        }
        float a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
    }
    if (t0 > t1) return false;
    const auto above = [&](float t) {
        const bromath::Vec3 p = origin + dir * t;
        return p.y - heightAt(p.x, p.z);
    };
    // Steps of a ring cell near the origin, growing with distance (a
    // hundredth of it): a ridge thinner than a step can be stepped over.
    // (A ray starting under the land hits nothing: no hit from below.)
    if (above(t0) <= 0.0f) return false;
    float prevT = t0;
    for (float t = t0; t < t1;) {
        t = std::min(t1, t + std::max(rings_.cellSize, 0.01f * t));
        const float v = above(t);
        if (v <= 0.0f) {
            float a = prevT, b = t;
            for (int i = 0; i < 40 && b - a > 0.01f; ++i) {
                const float m = 0.5f * (a + b);
                (above(m) > 0.0f ? a : b) = m;
            }
            dist = b;
            const bromath::Vec3 p = origin + dir * b;
            const float e = 0.5f * rings_.cellSize;
            normal = bromath::vnorm(bromath::Vec3{heightAt(p.x - e, p.z) - heightAt(p.x + e, p.z), 2.0f * e,
                                                  heightAt(p.x, p.z - e) - heightAt(p.x, p.z + e)});
            return true;
        }
        prevT = t;
    }
    return false;
}

SurfaceLayerConfig LandNode::tileConfig() const {
    SurfaceLayerConfig c;
    c.tileTexels = 64;
    // Two texels a cell for the finest ring: a vertex's height, normal and
    // colour read a level whose texel is half its cell (mesh_land.vert).
    c.tileMetres = rings_.cellSize * 0.5f * static_cast<float>(c.tileTexels);
    // Two texels of border: a normal's central differences at a tile's edge
    // read one past it, and the bilinear filter one more.
    c.border = 2;
    c.slots = 2048;
    c.pageSize = 256;
    c.maxLevels = 20;
    // The land's fragments read its albedo and normal at their own
    // footprint (mesh.frag): refine a tile while its texel covers more than
    // two and a half pixels. (Four left the land from the air a bilinear
    // blur of four pixels along the view and, where it slants, some seven
    // across it; two cost the planet's region view 0.18 ms of its land's
    // texture reads.)
    c.texelPixels = 2.5f;
    // The land's sun reads these tiles over its levels' squares about the
    // eye, out of the view as much as in it: there, as fine as its levels'
    // texels. (Held only by the coarsest out of the view, a ridge off to the
    // side stood a thousand-kilometre tile's heights and shaded the hollow
    // before the eye where a flight that had looked there left it lit.)
    c.shadeTexels = 0.5f * static_cast<float>(LandSun::kSide);
    c.shadeTexelMetres = LandSun::kTexel0;
    c.shadeReach = 0.5f * static_cast<float>(LandSun::kSide) * LandSun::kTexel0 *
                   static_cast<float>(1 << (LandSun::kLevels - 1));
    return c;
}

void LandNode::buildRings() {
    const int N = rings_.resolution;
    const int L = wantedRings();
    if (L == ringLevels_ && N == builtResolution_) return;
    ringLevels_ = L;
    builtResolution_ = N;
    const int row = N + 1;
    // Ring 0 is a full N x N grid of cells; ring l > 0 leaves out a central
    // block the ring inside it covers. The block is inset kHoleInset of
    // ring l's cells a side short of the inner ring's extent: each ring
    // snaps its centre to its own grid (twice its cell), so a ring and the
    // one inside it are offset by up to a cell, and the inset makes them
    // overlap instead of leaving a sliver of nothing between them (the
    // overlap draws the same surface twice: mesh_land.vert's morph).
    constexpr int kHoleInset = 2;
    const int holeQuads = N / 2 - 2 * kHoleInset;
    const int holeLo = N / 4 + kHoleInset, holeHi = holeLo + holeQuads;

    bromesh::MeshData m;
    m.positions.reserve(static_cast<size_t>(row) * row * L * 3);
    m.uvs.reserve(static_cast<size_t>(row) * row * L * 2);
    blocks_.clear();
    for (int l = 0; l < L; ++l) {
        const uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                // In cells from the ring's centre; the vertex stage scales
                // them by the ring's cell (rings_ may change it).
                m.positions.insert(m.positions.end(),
                                   {static_cast<float>(i - N / 2), 0.0f, static_cast<float>(j - N / 2)});
                m.normals.insert(m.normals.end(), {0.0f, 1.0f, 0.0f});
                // uv.x the ring; uv.y which axes the vertex is off the next
                // coarser ring's grid on (1 x, 2 z, 3 both), for the morph;
                // the coarsest ring has none to morph to.
                m.uvs.push_back(static_cast<float>(l));
                m.uvs.push_back(l + 1 < L ? static_cast<float>((i & 1) | ((j & 1) << 1)) : 0.0f);
                m.tangents.insert(m.tangents.end(), {1.0f, 0.0f, 0.0f, 1.0f});
            }
        // Block by block, each block's quads together.
        for (int bj = 0; bj < N; bj += kBlockCells)
            for (int bi = 0; bi < N; bi += kBlockCells) {
                Block b{l, bi, bj, std::min(bi + kBlockCells, N), std::min(bj + kBlockCells, N),
                        static_cast<uint32_t>(m.indices.size()), 0};
                for (int j = b.j0; j < b.j1; ++j)
                    for (int i = b.i0; i < b.i1; ++i) {
                        if (l > 0 && holeQuads > 0 && i >= holeLo && i < holeHi && j >= holeLo && j < holeHi)
                            continue;
                        const uint32_t v00 = base + static_cast<uint32_t>(j * row + i);
                        const uint32_t v10 = v00 + 1, v01 = v00 + static_cast<uint32_t>(row), v11 = v01 + 1;
                        // Counter-clockwise seen from above.
                        m.indices.insert(m.indices.end(), {v00, v01, v10, v10, v01, v11});
                    }
                b.indexCount = static_cast<uint32_t>(m.indices.size()) - b.firstIndex;
                if (b.indexCount) blocks_.push_back(b);
            }
    }
    setMesh(std::move(m));
}

bool LandNode::heightRange(float x0, float z0, float x1, float z1, float& lo, float& hi) const {
    if (round()) {
        // The body's whole range, and as far as the ground falls away from
        // the place origin to the rect's farthest corner.
        const double R = place_->body().radius;
        const double fx = std::max(std::fabs(x0), std::fabs(x1)), fz = std::max(std::fabs(z0), std::fabs(z1));
        const double rho = std::min(std::sqrt(fx * fx + fz * fz), R);
        lo = lowest() - static_cast<float>(R - std::sqrt(R * R - rho * rho));
        hi = highest();
        return !items_.empty();
    }
    const float e = extent_;
    if (items_.empty() || x1 < -e || z1 < -e || x0 > e || z0 > e) return false;
    const int n = itemCells_;
    const float side = 2.0f * e / static_cast<float>(n);
    const auto cellOf = [&](float v) { return std::clamp(static_cast<int>(std::floor((v + e) / side)), 0, n - 1); };
    const int i0 = cellOf(x0), i1 = cellOf(x1), j0 = cellOf(z0), j1 = cellOf(z1);
    lo = std::numeric_limits<float>::max();
    hi = -lo;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const bromath::AABB3& b = items_[static_cast<size_t>(j) * n + i].bounds;
            lo = std::min(lo, b.min.y);
            hi = std::max(hi, b.max.y);
        }
    return true;
}

bromath::AABB3 LandNode::blockBounds(const Block& b, const bromath::Vec3& eye, int first, bool* hidden) const {
    if (hidden) *hidden = false;
    const int ring = b.ring == 0 ? first : b.ring;
    const double cl = static_cast<double>(rings_.cellSize) * std::exp2(static_cast<double>(ring));
    const double snap = 2.0 * cl;
    // The eye on the land's map, its ring's centre snapped to the map's grid.
    const double e[3] = {eye.x, eye.y, eye.z};
    double ex, ez, eh;
    if (place_)
        place_->landToMap(e, ex, ez, eh);
    else
        ex = eye.x, ez = eye.z;
    const double cx = std::floor(ex / snap) * snap, cz = std::floor(ez / snap) * snap;
    const double half = 0.5 * static_cast<double>(rings_.resolution);
    const double reach = mapReach();
    const auto at = [&](double c, int i) { return std::clamp(c + (static_cast<double>(i) - half) * cl, -reach, reach); };
    const double x0 = at(cx, b.i0), x1 = at(cx, b.i1), z0 = at(cz, b.j0), z1 = at(cz, b.j1);
    if (!round())
        return {{static_cast<float>(x0), chartBounds_.min.y, static_cast<float>(z0)},
                {static_cast<float>(x1), highest(), static_cast<float>(z1)}};

    // On a body: the map rect's corners and middle at both heights, widened
    // by the ground's bulge past a chord across it (body_surface_map.cpp's).
    const double R = place_->body().radius;
    const double lo = lowest(), hi = highest();
    const double mx = 0.5 * (x0 + x1), mz = 0.5 * (z0 + z1);
    const double radius = 0.5 * std::hypot(x1 - x0, z1 - z0) / R;
    if (radius >= 0.5 * 3.14159265358979323846) return bounds();   // a hemisphere or more: the ball
    bromath::Vec3 mn{1e30f, 1e30f, 1e30f}, mxv{-1e30f, -1e30f, -1e30f};
    for (int c = 0; c < 5; ++c) {
        const double px = c < 4 ? ((c & 1) ? x1 : x0) : mx, pz = c < 4 ? ((c & 2) ? z1 : z0) : mz;
        for (const double h : {lo, hi}) {
            double p[3];
            place_->mapToLand(px, pz, h, p);
            const bromath::Vec3 v{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
            mn = bromath::vmin(mn, v);
            mxv = bromath::vmax(mxv, v);
        }
    }
    const float bulge = static_cast<float>((R + hi) * (1.0 - std::cos(radius))) + 1.0f;
    if (hidden) {
        double p[3], c[3], eb[3];
        place_->mapToLand(mx, mz, 0.0, p);
        place_->toBody(p, c);
        normalize3(c);
        place_->toBody(e, eb);
        *hidden = beyondLimb(place_->body(), eb, c, radius, lo, hi);
    }
    return {mn - bromath::Vec3{bulge, bulge, bulge}, mxv + bromath::Vec3{bulge, bulge, bulge}};
}

float LandNode::eyeAbove(const bromath::Vec3& eye) const {
    if (!round()) return eye.y - heightAt(eye.x, eye.z);
    double b[3];
    place_->toBody(eye, b);
    const double r = std::sqrt(dot3(b, b));
    for (double& v : b) v /= r;
    return static_cast<float>(r - place_->body().radius - heightOver(b));
}

int LandNode::firstRing(const bromath::Vec3& eye, float focalPixels) const {
    // The eye's height over the ground under it: every cell is at least
    // that far, so a ring whose cell is under a pixel there is under a
    // pixel everywhere.
    const double above = eyeAbove(eye);
    int first = 0;
    while (first + 1 < ringLevels_ &&
           static_cast<double>(rings_.cellSize) * std::exp2(static_cast<double>(first)) * focalPixels < above)
        ++first;
    return first;
}

namespace {

// Every item of a land, from `in` (its generation 0: the caller's).
void buildFaceItems(const LandNode::ItemInputs& in, std::vector<SurfaceItem>& items_, int& itemCells_,
                    bromath::AABB3& chartBounds_);

void buildItems(const LandNode::ItemInputs& in, std::vector<SurfaceItem>& items_, int& itemCells_,
                bromath::AABB3& chartBounds_) {
    if (in.round) {
        buildFaceItems(in, items_, itemCells_, chartBounds_);
        return;
    }
    const FieldRef& height_ = in.height;
    constexpr uint64_t generation_ = 0;
    items_.clear();
    const float e = in.extent;
    const int cells = std::clamp(static_cast<int>(std::ceil(2.0f * e / kItemMetres)), 1, kItemCells);
    itemCells_ = cells;
    const float side = 2.0f * e / static_cast<float>(cells);
    // Every cell's samples at once (FastNoise2's batch over a grid).
    const int per = kItemSamples;
    const int cols = cells * (per - 1) + 1;
    const float step = side / static_cast<float>(per - 1);
    std::vector<float> h(static_cast<size_t>(cols) * cols, 0.0f);
    std::vector<float> xs(h.size()), zs(h.size());
    for (int j = 0; j < cols; ++j)
        for (int i = 0; i < cols; ++i) {
            xs[static_cast<size_t>(j) * cols + i] = -e + static_cast<float>(i) * step;
            zs[static_cast<size_t>(j) * cols + i] = -e + static_cast<float>(j) * step;
        }
    if (height_) height_->sample(xs.data(), zs.data(), static_cast<int>(h.size()), h.data());
    if (in.base)
        for (size_t k = 0; k < h.size(); ++k) h[k] = in.overTree(h[k], 0, xs[k], zs[k]);
    chartBounds_ = bromath::aempty3();
    items_.reserve(static_cast<size_t>(cells) * cells);
    for (int cz = 0; cz < cells; ++cz)
        for (int cx = 0; cx < cells; ++cx) {
            float lo = std::numeric_limits<float>::max(), hi = -lo;
            for (int j = 0; j < per; ++j)
                for (int i = 0; i < per; ++i) {
                    const float v = h[static_cast<size_t>(cz * (per - 1) + j) * cols + cx * (per - 1) + i];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
            const float pad = (hi - lo) * kItemMargin + 1.0f;
            const bromath::AABB3 box{{-e + cx * side, lo - pad, -e + cz * side},
                                     {-e + (cx + 1) * side, hi + pad, -e + (cz + 1) * side}};
            items_.push_back({box, generation_});
            chartBounds_.min = bromath::vmin(chartBounds_.min, box.min);
            chartBounds_.max = bromath::vmax(chartBounds_.max, box.max);
        }
}

void buildFaceItems(const LandNode::ItemInputs& in, std::vector<SurfaceItem>& items_, int& itemCells_,
                    bromath::AABB3& chartBounds_) {
    const FieldRef& height_ = in.height;
    constexpr uint64_t generation_ = 0;
    items_.clear();
    const Body& body = in.body;
    const double R = body.radius;
    const double e = faceHalf(body);
    const int cells = kFaceCells;
    itemCells_ = cells;
    const double side = 2.0 * e / cells;
    const int per = kItemSamples;
    const int cols = cells * (per - 1) + 1;
    const double step = side / (per - 1);
    const size_t n = static_cast<size_t>(cols) * cols;
    std::vector<float> h(n, 0.0f), xs(n), ys(n), zs(n);
    chartBounds_ = bromath::aempty3();
    items_.reserve(static_cast<size_t>(cells) * cells * 6);
    for (int f = 0; f < 6; ++f) {
        // The face's samples on its datum, relative to the face's centre.
        if (!height_) std::fill(h.begin(), h.end(), 0.0f);
        if (height_) {
            const CubeFace& F = cubeFace(f);
            const double o[3] = {R * F.n[0], R * F.n[1], R * F.n[2]};
            for (int j = 0; j < cols; ++j)
                for (int i = 0; i < cols; ++i) {
                    double d[3];
                    dirOf(body, {f, -e + i * step, -e + j * step}, d);
                    const size_t k = static_cast<size_t>(j) * cols + i;
                    xs[k] = static_cast<float>(R * d[0] - o[0]);
                    ys[k] = static_cast<float>(R * d[1] - o[1]);
                    zs[k] = static_cast<float>(R * d[2] - o[2]);
                }
            height_->sample(xs.data(), ys.data(), zs.data(), static_cast<int>(n), o, h.data());
        }
        if (in.base)
            for (int j = 0; j < cols; ++j)
                for (int i = 0; i < cols; ++i) {
                    float& v = h[static_cast<size_t>(j) * cols + i];
                    v = in.overTree(v, f, -e + i * step, -e + j * step);
                }
        for (int cz = 0; cz < cells; ++cz)
            for (int cx = 0; cx < cells; ++cx) {
                float lo = std::numeric_limits<float>::max(), hi = -lo;
                for (int j = 0; j < per; ++j)
                    for (int i = 0; i < per; ++i) {
                        const float v = h[static_cast<size_t>(cz * (per - 1) + j) * cols + cx * (per - 1) + i];
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                    }
                const float pad = (hi - lo) * kItemMargin + 1.0f;
                const bromath::AABB3 box{
                    {static_cast<float>(-e + cx * side), lo - pad, static_cast<float>(-e + cz * side)},
                    {static_cast<float>(-e + (cx + 1) * side), hi + pad, static_cast<float>(-e + (cz + 1) * side)}};
                items_.push_back({box, generation_, static_cast<uint8_t>(f)});
                chartBounds_.min = bromath::vmin(chartBounds_.min, box.min);
                chartBounds_.max = bromath::vmax(chartBounds_.max, box.max);
            }
    }
}

// Item `k`'s height range from `in` (of `cells` a side a root).
void itemRange(const LandNode::ItemInputs& in, int cells, size_t k, float& lo, float& hi) {
    const int per = kItemSamples;
    const int root = static_cast<int>(k / (static_cast<size_t>(cells) * cells));
    const int cz = static_cast<int>(k / cells % cells), cx = static_cast<int>(k % cells);
    const size_t n = static_cast<size_t>(per) * per;
    std::vector<float> h(n, 0.0f), xs(n), ys(n), zs(n);
    const double e = in.round ? faceHalf(in.body) : static_cast<double>(in.extent);
    const double side = 2.0 * e / cells, step = side / (per - 1);
    const double ox = -e + cx * side, oz = -e + cz * side;
    if (in.round) {
        const Body& body = in.body;
        const double R = body.radius;
        const CubeFace& F = cubeFace(root);
        const double o[3] = {R * F.n[0], R * F.n[1], R * F.n[2]};
        for (int j = 0; j < per; ++j)
            for (int i = 0; i < per; ++i) {
                double d[3];
                dirOf(body, {root, ox + i * step, oz + j * step}, d);
                const size_t s = static_cast<size_t>(j) * per + i;
                xs[s] = static_cast<float>(R * d[0] - o[0]);
                ys[s] = static_cast<float>(R * d[1] - o[1]);
                zs[s] = static_cast<float>(R * d[2] - o[2]);
            }
        if (in.height) in.height->sample(xs.data(), ys.data(), zs.data(), static_cast<int>(n), o, h.data());
    } else {
        for (int j = 0; j < per; ++j)
            for (int i = 0; i < per; ++i) {
                xs[static_cast<size_t>(j) * per + i] = static_cast<float>(ox + i * step);
                zs[static_cast<size_t>(j) * per + i] = static_cast<float>(oz + j * step);
            }
        if (in.height) in.height->sample(xs.data(), zs.data(), static_cast<int>(n), h.data());
    }
    lo = std::numeric_limits<float>::max();
    hi = -lo;
    for (int j = 0; j < per; ++j)
        for (int i = 0; i < per; ++i) {
            const float v = in.overTree(h[static_cast<size_t>(j) * per + i], root, ox + i * step, oz + j * step);
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
}

// Items a refresh job makes: about a few milliseconds of a worker's time.
constexpr size_t kItemsPerBatch = 512;

}  // namespace

LandNode::ItemInputs LandNode::itemInputs() const {
    ItemInputs in;
    in.height = height_;
    in.base = base_;
    in.regions = regionStore_->snapshot();
    in.round = round();
    if (in.round) in.body = place_->body();
    in.extent = extent_;
    return in;
}

void LandNode::rebuildItems(bool now) {
    if (now) {
        // Every batch asked before is of items made again here.
        itemEpoch_ = itemsAsked_;
        buildItems(itemInputs(), items_, itemCells_, chartBounds_);
        for (SurfaceItem& it : items_) it.generation = generation_;
        // (Where the canopy stands for the plants, the land rises by it.)
        if (round() || !bromath::aisEmpty(chartBounds_)) chartBounds_.max.y += canopyTop_;
        map_.set(place_, lowest());
        return;
    }
    ItemBatch b;
    b.seq = ++itemsAsked_;
    b.whole = true;
    WorldWorkers::shared().post([in = itemInputs(), b = std::move(b), inbox = itemInbox_]() mutable {
        buildItems(in, b.items, b.cells, b.chart);
        std::lock_guard<std::mutex> lock(inbox->mutex);
        inbox->done.push_back(std::move(b));
        inbox->doneCv.notify_all();
    });
}

void LandNode::refreshItems(std::vector<size_t> ks, uint64_t generation) {
    if (ks.empty()) return;
    // Stale now: the CPU's heights moved this frame and the fills read the
    // regions as they are this frame, so the tiles are made again at once;
    // only the boxes (how high and low the items reach) wait on the
    // workers. (Their tiles waited the nine frames a batch took, the eye
    // set on the new heights standing under the land drawn as it was: a
    // white floor at the landing.)
    for (const size_t k : ks)
        if (k < items_.size()) items_[k].generation = generation;
    ++itemsSerial_;
    const ItemInputs in = itemInputs();
    for (size_t at = 0; at < ks.size(); at += kItemsPerBatch) {
        ItemBatch b;
        b.seq = ++itemsAsked_;
        b.generation = generation;
        b.ks.assign(ks.begin() + static_cast<std::ptrdiff_t>(at),
                    ks.begin() + static_cast<std::ptrdiff_t>(std::min(at + kItemsPerBatch, ks.size())));
        WorldWorkers::shared().post([in, cells = itemCells_, b = std::move(b), inbox = itemInbox_]() mutable {
            b.lo.resize(b.ks.size());
            b.hi.resize(b.ks.size());
            for (size_t i = 0; i < b.ks.size(); ++i) itemRange(in, cells, b.ks[i], b.lo[i], b.hi[i]);
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->done.push_back(std::move(b));
            inbox->doneCv.notify_all();
        });
    }
}

void LandNode::takeItems() {
    if (itemsApplied_ == itemsAsked_) return;
    {
        std::lock_guard<std::mutex> lock(itemInbox_->mutex);
        for (ItemBatch& b : itemInbox_->done) {
            const uint64_t seq = b.seq;
            itemReady_.emplace(seq, std::move(b));
        }
        itemInbox_->done.clear();
    }
    // In the order asked: a later batch's boxes grow what an earlier made.
    bool changed = false;
    for (auto it = itemReady_.begin(); it != itemReady_.end() && it->first == itemsApplied_ + 1;
         it = itemReady_.erase(it)) {
        ++itemsApplied_;
        if (it->first <= itemEpoch_) continue;   // its items made again at once since
        applyItems(it->second);
        changed = true;
    }
    if (changed) ++itemsSerial_;
}

void LandNode::waitBaseItems() {
    // (The batches up to the base's are applied in order: every one of them
    // done, in the inbox or waiting on an earlier one.)
    while (itemsApplied_ < baseItems_) {
        {
            std::unique_lock<std::mutex> lock(itemInbox_->mutex);
            itemInbox_->doneCv.wait(lock, [&] {
                return itemsApplied_ + itemReady_.size() + itemInbox_->done.size() >= baseItems_;
            });
        }
        takeItems();
    }
}

void LandNode::applyItems(ItemBatch& b) {
    if (b.whole) {
        // Its heights (and so its tiles' boxes) are the tree's over the base.
        ++generation_;
        items_ = std::move(b.items);
        itemCells_ = b.cells;
        chartBounds_ = b.chart;
        for (SurfaceItem& it : items_) it.generation = generation_;
        if (round() || !bromath::aisEmpty(chartBounds_)) chartBounds_.max.y += canopyTop_;
        map_.set(place_, lowest());
        return;
    }
    const size_t n = std::min({b.ks.size(), b.lo.size(), b.hi.size()});
    for (size_t i = 0; i < n; ++i) {
        if (b.ks[i] >= items_.size()) continue;
        SurfaceItem& it = items_[b.ks[i]];
        const float pad = (b.hi[i] - b.lo[i]) * kItemMargin + 1.0f;
        it.bounds.min.y = std::min(it.bounds.min.y, b.lo[i] - pad);
        it.bounds.max.y = std::max(it.bounds.max.y, b.hi[i] + pad);
        it.generation = b.generation;
        chartBounds_.min.y = std::min(chartBounds_.min.y, it.bounds.min.y);
        chartBounds_.max.y = std::max(chartBounds_.max.y, it.bounds.max.y + canopyTop_);
    }
}

}  // namespace bro::scene
