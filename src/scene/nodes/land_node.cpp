#include "scene/nodes/land_node.h"

#include "scene/nodes/world_module.h"

#include <algorithm>
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

void LandNode::setExtent(float extent) {
    if (!(extent > 0.0f)) return;
    extent_ = extent;
    ++generation_;
    rebuildItems();
}

void LandNode::setRings(const Rings& rings) {
    Rings r = rings;
    // The rings tile exactly only when a ring's hole is a whole number of
    // cells inset (buildRings), so the resolution is a multiple of 4.
    r.resolution = std::clamp(r.resolution / 4 * 4, 8, 1024);
    r.levels = std::clamp(r.levels, 1, 20);
    if (!(r.cellSize > 0.0f)) r.cellSize = rings_.cellSize;
    const bool geometry = r.resolution != rings_.resolution || r.levels != rings_.levels;
    rings_ = r;
    if (geometry) buildRings();
    ++generation_;
}

float LandNode::heightAt(float x, float z) const { return height_ ? height_->at(x, z) : 0.0f; }

void LandNode::sampleHeights(float x0, float z0, float cell, int cols, int rows, float* out) const {
    const size_t n = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    std::vector<float> xs(n), zs(n);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            xs[static_cast<size_t>(j) * cols + i] = x0 + static_cast<float>(i) * cell;
            zs[static_cast<size_t>(j) * cols + i] = z0 + static_cast<float>(j) * cell;
        }
    if (height_) {
        height_->sample(xs.data(), zs.data(), static_cast<int>(n), out);
    } else {
        std::fill(out, out + n, 0.0f);
    }
    for (size_t k = 0; k < n; ++k)
        if (std::fabs(xs[k]) > extent_ || std::fabs(zs[k]) > extent_) out[k] = std::numeric_limits<float>::quiet_NaN();
}

bool LandNode::raycast(const bromath::Vec3& origin, const bromath::Vec3& dir, float maxDist, float& dist,
                       bromath::Vec3& normal) const {
    // The span of the ray over the land's box.
    float t0 = 0.0f, t1 = maxDist;
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
    const float lo[3] = {bounds_.min.x, bounds_.min.y, bounds_.min.z}, hi[3] = {bounds_.max.x, bounds_.max.y, bounds_.max.z};
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
    c.slots = 1024;
    c.pageSize = 256;
    c.maxLevels = 20;
    // A ring cell is about 2 / resolution of the view's height across, two
    // texels to it: refine a tile while its texel covers more than that.
    c.texelPixels = 8.0f;
    return c;
}

void LandNode::buildRings() {
    const int N = rings_.resolution;
    const int L = rings_.levels;
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

bromath::AABB3 LandNode::blockBounds(const Block& b, const bromath::Vec3& eye) const {
    const float cl = rings_.cellSize * std::exp2(static_cast<float>(b.ring));
    const float snap = 2.0f * cl;
    const float cx = std::floor(eye.x / snap) * snap, cz = std::floor(eye.z / snap) * snap;
    const float half = 0.5f * static_cast<float>(rings_.resolution);
    const auto at = [&](float c, int i) {
        return std::clamp(c + (static_cast<float>(i) - half) * cl, -extent_, extent_);
    };
    return {{at(cx, b.i0), bounds_.min.y, at(cz, b.j0)}, {at(cx, b.i1), bounds_.max.y, at(cz, b.j1)}};
}

void LandNode::rebuildItems() {
    items_.clear();
    const float e = extent_;
    const int cells = std::clamp(static_cast<int>(std::ceil(2.0f * e / kItemMetres)), 1, kItemCells);
    itemCells_ = cells;
    const float side = 2.0f * e / static_cast<float>(cells);
    // Every cell's samples at once (FastNoise2's batch over a grid).
    const int per = kItemSamples;
    const int cols = cells * (per - 1) + 1;
    const float step = side / static_cast<float>(per - 1);
    std::vector<float> h(static_cast<size_t>(cols) * cols, 0.0f);
    if (height_) {
        std::vector<float> xs(h.size()), zs(h.size());
        for (int j = 0; j < cols; ++j)
            for (int i = 0; i < cols; ++i) {
                xs[static_cast<size_t>(j) * cols + i] = -e + static_cast<float>(i) * step;
                zs[static_cast<size_t>(j) * cols + i] = -e + static_cast<float>(j) * step;
            }
        height_->sample(xs.data(), zs.data(), static_cast<int>(h.size()), h.data());
    }
    bounds_ = bromath::aempty3();
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
            bounds_.min = bromath::vmin(bounds_.min, box.min);
            bounds_.max = bromath::vmax(bounds_.max, box.max);
        }
}

}  // namespace bro::scene
