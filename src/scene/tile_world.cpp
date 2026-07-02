#include "scene/tile_world.h"

#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/scene_node.h"

#include "tile/autotile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace bro::scene {

using bromath::Vec3;

// -------------------------------------------------------------------------
// Mesh helper — append a quad with the four corners given in any order and an
// explicit outward normal; the two triangles are wound to match `n` so the
// caller never has to reason about winding.
// -------------------------------------------------------------------------

static void addQuad(bromesh::MeshData& m,
                    const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                    const Vec3& n,
                    const float ca[4], const float cb[4],
                    const float cc[4], const float cd[4],
                    const float* uvs /* 8 floats (per-corner u,v) or nullptr */) {
    // Geometric normal of triangle (a,b,c); flip emission order if it opposes n.
    Vec3 e0{b.x - a.x, b.y - a.y, b.z - a.z};
    Vec3 e1{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3 gn{e0.y * e1.z - e0.z * e1.y,
            e0.z * e1.x - e0.x * e1.z,
            e0.x * e1.y - e0.y * e1.x};
    bool flip = (gn.x * n.x + gn.y * n.y + gn.z * n.z) < 0.0f;

    uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
    const Vec3* vs[4] = {&a, &b, &c, &d};
    const float* cs[4] = {ca, cb, cc, cd};
    for (int i = 0; i < 4; ++i) {
        m.positions.push_back(vs[i]->x);
        m.positions.push_back(vs[i]->y);
        m.positions.push_back(vs[i]->z);
        m.normals.push_back(n.x);
        m.normals.push_back(n.y);
        m.normals.push_back(n.z);
        m.colors.push_back(cs[i][0]);
        m.colors.push_back(cs[i][1]);
        m.colors.push_back(cs[i][2]);
        m.colors.push_back(cs[i][3]);
        if (uvs) {
            m.uvs.push_back(uvs[i * 2 + 0]);
            m.uvs.push_back(uvs[i * 2 + 1]);
        }
    }
    if (!flip) {
        m.indices.insert(m.indices.end(),
            {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
    } else {
        m.indices.insert(m.indices.end(),
            {base + 0, base + 2, base + 1, base + 0, base + 3, base + 2});
    }
}

// Triangle fan from pts[0] across a convex, planar polygon (`count` >= 3), winding
// auto-fixed against `n` the same way addQuad does (checked once on the first
// triangle and reused, since a planar convex fan's winding is uniform).
static void addFan(bromesh::MeshData& m, const Vec3* pts, const float* const* cols,
                   const float* uvs /* 2*count floats or nullptr */, int count,
                   const Vec3& n) {
    Vec3 e0{pts[1].x - pts[0].x, pts[1].y - pts[0].y, pts[1].z - pts[0].z};
    Vec3 e1{pts[2].x - pts[0].x, pts[2].y - pts[0].y, pts[2].z - pts[0].z};
    Vec3 gn{e0.y * e1.z - e0.z * e1.y,
            e0.z * e1.x - e0.x * e1.z,
            e0.x * e1.y - e0.y * e1.x};
    bool flip = (gn.x * n.x + gn.y * n.y + gn.z * n.z) < 0.0f;

    uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
    for (int i = 0; i < count; ++i) {
        m.positions.push_back(pts[i].x);
        m.positions.push_back(pts[i].y);
        m.positions.push_back(pts[i].z);
        m.normals.push_back(n.x);
        m.normals.push_back(n.y);
        m.normals.push_back(n.z);
        m.colors.push_back(cols[i][0]);
        m.colors.push_back(cols[i][1]);
        m.colors.push_back(cols[i][2]);
        m.colors.push_back(cols[i][3]);
        if (uvs) {
            m.uvs.push_back(uvs[i * 2 + 0]);
            m.uvs.push_back(uvs[i * 2 + 1]);
        }
    }
    for (int i = 1; i + 1 < count; ++i) {
        if (!flip)
            m.indices.insert(m.indices.end(), {base, base + i, base + static_cast<uint32_t>(i + 1)});
        else
            m.indices.insert(m.indices.end(), {base, base + static_cast<uint32_t>(i + 1), base + i});
    }
}

// -------------------------------------------------------------------------
// Hex geometry — pointy-top regular hexagons, cellSize == circumradius/edge
// length R. Corner i sits between canonical hex neighbour directions i-1 and i
// (see coord.h / coord.cpp kHexAxial), at angle (30 - 60*i) degrees:
//   corner[i] = R * (cos(30-60i deg), sin(30-60i deg))
// Direction unit vectors (independent of R) follow the same angle convention,
// used for cliff-face outward normals.
// -------------------------------------------------------------------------

namespace {
struct Vec2 { float x, z; };

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

// Computed at namespace-init time (not constexpr — std::cos/sin aren't constexpr
// pre-C++23 in this toolchain); cheap, done once.
const std::array<Vec2, 6> kHexCorner = [] {
    std::array<Vec2, 6> c{};
    for (int i = 0; i < 6; ++i) {
        float a = (30.0f - 60.0f * static_cast<float>(i)) * kDeg2Rad;
        c[i] = {std::cos(a), std::sin(a)};
    }
    return c;
}();

const std::array<Vec2, 6> kHexDir = [] {
    std::array<Vec2, 6> d{};
    for (int i = 0; i < 6; ++i) {
        float a = (-60.0f * static_cast<float>(i)) * kDeg2Rad;
        d[i] = {std::cos(a), std::sin(a)};
    }
    return d;
}();

constexpr float kHexSqrt3 = 1.7320508075688772f;
}  // namespace

// -------------------------------------------------------------------------
// TileWorld
// -------------------------------------------------------------------------

TileWorld::TileWorld(SceneGraph& graph) : graph_(graph) {}

TileWorld::~TileWorld() { clear(); }

bool TileWorld::solid(int x, int y) const {
    if (!grid_ || !grid_->inBounds({x, y})) return false;
    return grid_->tile(0, {x, y}) != 0;
}

float TileWorld::topY(int x, int y) const {
    return static_cast<float>(grid_->elevation({x, y})) * config_.heightStep;
}

void TileWorld::cellCenterLocal(int x, int y, float& px, float& pz) const {
    const float R = config_.cellSize;
    if (grid_ && grid_->topology() == tile::Topology::Hex) {
        tile::Hex h = tile::toHex({x, y});
        px = R * kHexSqrt3 * (static_cast<float>(h.q) + static_cast<float>(h.r) * 0.5f);
        pz = R * 1.5f * static_cast<float>(h.r);
        return;
    }
    px = (static_cast<float>(x) + 0.5f) * R;
    pz = (static_cast<float>(y) + 0.5f) * R;
}

tile::Cell TileWorld::pixelToHexCell(float lx, float lz) const {
    const float R = (config_.cellSize > 1e-8f) ? config_.cellSize : 1.0f;
    // Inverse of px = R*sqrt3*(q+r/2), pz = R*1.5*r.
    double r = static_cast<double>(lz) / (1.5 * R);
    double q = static_cast<double>(lx) / (kHexSqrt3 * R) - r * 0.5;
    // Cube-round to the nearest valid hex (q+r+s == 0 in cube space).
    double x = q, z = r, y = -x - z;
    double rx = std::round(x), ry = std::round(y), rz = std::round(z);
    double dx = std::fabs(rx - x), dy = std::fabs(ry - y), dz = std::fabs(rz - z);
    if (dx > dy && dx > dz) rx = -ry - rz;
    else if (dy > dz)       ry = -rx - rz;
    else                    rz = -rx - ry;
    return tile::fromHex(tile::Hex{static_cast<int>(rx), static_cast<int>(rz)});
}

void TileWorld::cellCenterWorldXZ(int x, int y, float& outX, float& outZ) const {
    float px = 0, pz = 0;
    cellCenterLocal(x, y, px, pz);
    outX = config_.origin.x + px;
    outZ = config_.origin.z + pz;
}

void TileWorld::worldBounds(float& minX, float& minZ, float& maxX, float& maxZ) const {
    const float R = config_.cellSize;
    if (!grid_ || grid_->topology() != tile::Topology::Hex) {
        minX = config_.origin.x;
        minZ = config_.origin.z;
        maxX = config_.origin.x + static_cast<float>(config_.width) * R;
        maxZ = config_.origin.z + static_cast<float>(config_.height) * R;
        return;
    }
    // Hex: sweep every border cell's actual hex corners (cheap — border only) so the
    // box isn't clipped by cell centers alone.
    minX = minZ = std::numeric_limits<float>::infinity();
    maxX = maxZ = -std::numeric_limits<float>::infinity();
    const int W = config_.width, H = config_.height;
    auto sweep = [&](int x, int y) {
        float cx = 0, cz = 0;
        cellCenterLocal(x, y, cx, cz);
        for (int i = 0; i < 6; ++i) {
            float px = cx + R * kHexCorner[i].x;
            float pz = cz + R * kHexCorner[i].z;
            minX = std::min(minX, px); maxX = std::max(maxX, px);
            minZ = std::min(minZ, pz); maxZ = std::max(maxZ, pz);
        }
    };
    for (int x = 0; x < W; ++x) { sweep(x, 0); sweep(x, H - 1); }
    for (int y = 0; y < H; ++y) { sweep(0, y); sweep(W - 1, y); }
    minX += config_.origin.x; maxX += config_.origin.x;
    minZ += config_.origin.z; maxZ += config_.origin.z;
}

int TileWorld::atlasLayerCell(int x, int y, int layer, uint16_t id) const {
    // Find an autotile rule for this (id, layer) — rule lists are tiny.
    const TileWorldConfig::AutotileRule* rule = nullptr;
    for (const auto& r : config_.autotiles) {
        if (r.id == id && r.layer == layer) { rule = &r; break; }
    }
    if (!rule || rule->cells.empty())
        return atlasCellFor(id);

    using namespace tile;
    FamilyFn fam = (rule->family == TileWorldConfig::AutotileFamily::NonEmpty)
                       ? familyNonEmpty(layer)
                       : familyTile(layer, id);
    Cell c{x, y};
    int variant = 0;
    switch (rule->mode) {
        case TileWorldConfig::AutotileMode::Edge:
            variant = edgeMask(*grid_, c, fam);
            break;
        case TileWorldConfig::AutotileMode::Blob47:
            variant = blob47(blobMask(*grid_, c, fam));
            break;
        case TileWorldConfig::AutotileMode::Wang:
            variant = wangCorners(*grid_, c, fam);
            break;
    }
    if (variant >= 0 && variant < static_cast<int>(rule->cells.size()))
        return rule->cells[variant];
    return atlasCellFor(id);
}

void TileWorld::cellTint(int x, int y, float out[4]) const {
    out[0] = out[1] = out[2] = out[3] = 1.0f;
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return;
    size_t idx = static_cast<size_t>(y) * config_.width + x;
    if (idx >= tint_.size()) return;
    uint32_t t = tint_[idx];
    out[0] = ((t >> 24) & 0xFF) / 255.0f;
    out[1] = ((t >> 16) & 0xFF) / 255.0f;
    out[2] = ((t >> 8)  & 0xFF) / 255.0f;
    out[3] = ( t        & 0xFF) / 255.0f;
}

void TileWorld::atlasCellRect(int cell, float& u0, float& u1,
                              float& v0, float& v1) const {
    int cols = std::max(1, config_.atlasColumns);
    int rows = std::max(1, config_.atlasRows);
    if (cell < 0) cell = 0;
    int col = cell % cols;
    int row = (cell / cols) % rows;
    float du = 1.0f / static_cast<float>(cols);
    float dv = 1.0f / static_cast<float>(rows);
    // Inset by atlasInset texels on each side to fight cell-to-cell bleeding.
    float iu = (config_.atlasWidth  > 0) ? config_.atlasInset / config_.atlasWidth  : 0.0f;
    float iv = (config_.atlasHeight > 0) ? config_.atlasInset / config_.atlasHeight : 0.0f;
    u0 = col * du + iu;  u1 = (col + 1) * du - iu;
    v0 = row * dv + iv;  v1 = (row + 1) * dv - iv;
}

void TileWorld::configure(const TileWorldConfig& cfg) {
    clear();
    config_ = cfg;
    config_.width     = std::max(1, config_.width);
    config_.height    = std::max(1, config_.height);
    config_.chunkSize = std::max(1, config_.chunkSize);
    if (config_.layers.empty()) config_.layers = {"ground"};

    grid_ = std::make_unique<tile::TileGrid>(config_.width, config_.height,
                                             config_.topology, config_.layers);
    initFromGrid();
}

void TileWorld::loadGrid(tile::TileGrid&& newGrid) {
    clear();
    config_.width     = newGrid.width();
    config_.height    = newGrid.height();
    config_.topology  = newGrid.topology();
    config_.layers    = newGrid.layerNames();
    config_.chunkSize = std::max(1, config_.chunkSize);

    grid_ = std::make_unique<tile::TileGrid>(std::move(newGrid));
    initFromGrid();
}

void TileWorld::initFromGrid() {
    chunksX_ = (config_.width  + config_.chunkSize - 1) / config_.chunkSize;
    chunksY_ = (config_.height + config_.chunkSize - 1) / config_.chunkSize;
    chunks_.assign(static_cast<size_t>(chunksX_) * chunksY_, Chunk{});
    chunkAnimated_.assign(chunks_.size(), 0);

    tint_.assign(static_cast<size_t>(config_.width) * config_.height, 0xFFFFFFFFu);

    // Build the tile-id -> animation index map and reset frame state.
    animClock_ = 0.0;
    animFrame_.assign(config_.animations.size(), 0);
    animOf_.clear();
    for (size_t i = 0; i < config_.animations.size(); ++i) {
        uint16_t id = config_.animations[i].id;
        if (id >= animOf_.size()) animOf_.resize(id + 1, -1);
        animOf_[id] = static_cast<int>(i);
    }

    root_ = graph_.createNode("tileworld");
    graph_.root()->addChild(root_);
    root_->setPosition(config_.origin);

    rebuildAll();
}

void TileWorld::clear() {
    for (auto& c : chunks_) {
        if (c.ground) graph_.destroyNode(c.ground);
        for (auto* ov : c.overlays) if (ov) graph_.destroyNode(ov);
    }
    chunks_.clear();
    for (auto& k : objectKinds_)
        if (k.node) graph_.destroyNode(k.node);
    objectKinds_.clear();
    if (root_) {
        graph_.destroyNode(root_);
        root_ = nullptr;
    }
    grid_.reset();
    tint_.clear();
    chunkAnimated_.clear();
    animOf_.clear();
    animFrame_.clear();
    animClock_ = 0.0;
    chunksX_ = chunksY_ = 0;
}

// ---- authoring ----------------------------------------------------------

void TileWorld::markCellDirty(int x, int y) {
    if (!grid_) return;
    const int cs = config_.chunkSize;
    // The cell's own chunk plus all 8 neighbours' chunks — a height/tile change
    // alters cliffs and corner-AO of adjacent cells, which may live in adjacent
    // chunks.
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= config_.width || ny >= config_.height) continue;
            int ccx = nx / cs, ccy = ny / cs;
            chunks_[chunkIdx(ccx, ccy)].dirty = true;
        }
}

void TileWorld::setTile(int x, int y, uint16_t id, int layer) {
    if (!grid_ || !grid_->inBounds({x, y})) return;
    if (layer < 0 || layer >= grid_->layerCount()) return;
    grid_->setTile(layer, {x, y}, id);
    markCellDirty(x, y);
}

void TileWorld::setElevation(int x, int y, int level) {
    if (!grid_ || !grid_->inBounds({x, y})) return;
    grid_->setElevation({x, y}, static_cast<int16_t>(level));
    markCellDirty(x, y);
}

void TileWorld::setFlag(int x, int y, uint32_t bit, bool on) {
    if (!grid_ || !grid_->inBounds({x, y})) return;
    grid_->setFlag({x, y}, bit, on);
    // Flags don't affect geometry; no dirty needed.
}

void TileWorld::fillTile(int x0, int y0, int x1, int y1, uint16_t id, int layer) {
    if (!grid_) return;
    if (layer < 0 || layer >= grid_->layerCount()) return;
    grid_->fillRect(layer, {x0, y0}, {x1, y1}, id);
    int lo_x = std::min(x0, x1), hi_x = std::max(x0, x1);
    int lo_y = std::min(y0, y1), hi_y = std::max(y0, y1);
    for (int y = lo_y; y <= hi_y; ++y)
        for (int x = lo_x; x <= hi_x; ++x)
            markCellDirty(x, y);
}

void TileWorld::fillElevation(int x0, int y0, int x1, int y1, int level) {
    if (!grid_) return;
    int lo_x = std::min(x0, x1), hi_x = std::max(x0, x1);
    int lo_y = std::min(y0, y1), hi_y = std::max(y0, y1);
    for (int y = lo_y; y <= hi_y; ++y)
        for (int x = lo_x; x <= hi_x; ++x) {
            if (grid_->inBounds({x, y})) {
                grid_->setElevation({x, y}, static_cast<int16_t>(level));
                markCellDirty(x, y);
            }
        }
}

static uint32_t packRGBA(float r, float g, float b, float a) {
    auto u8 = [](float v) -> uint32_t {
        int n = static_cast<int>(v * 255.0f + 0.5f);
        return static_cast<uint32_t>(n < 0 ? 0 : (n > 255 ? 255 : n));
    };
    return (u8(r) << 24) | (u8(g) << 16) | (u8(b) << 8) | u8(a);
}

void TileWorld::setTint(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return;
    size_t idx = static_cast<size_t>(y) * config_.width + x;
    if (idx >= tint_.size()) return;
    tint_[idx] = packRGBA(r, g, b, a);
    markCellDirty(x, y);
}

void TileWorld::fillTint(int x0, int y0, int x1, int y1,
                         float r, float g, float b, float a) {
    uint32_t packed = packRGBA(r, g, b, a);
    int lo_x = std::min(x0, x1), hi_x = std::max(x0, x1);
    int lo_y = std::min(y0, y1), hi_y = std::max(y0, y1);
    for (int y = lo_y; y <= hi_y; ++y)
        for (int x = lo_x; x <= hi_x; ++x) {
            if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) continue;
            tint_[static_cast<size_t>(y) * config_.width + x] = packed;
            markCellDirty(x, y);
        }
}

// ---- query --------------------------------------------------------------

uint16_t TileWorld::tile(int x, int y, int layer) const {
    if (!grid_ || layer < 0 || layer >= grid_->layerCount()) return 0;
    return grid_->tile(layer, {x, y});
}

int TileWorld::elevation(int x, int y) const {
    return grid_ ? grid_->elevation({x, y}) : 0;
}

bool TileWorld::hasFlag(int x, int y, uint32_t bit) const {
    return grid_ ? grid_->hasFlag({x, y}, bit) : false;
}

bool TileWorld::worldToCell(float wx, float wz, int& outX, int& outY) const {
    if (!grid_) return false;
    float lx = wx - config_.origin.x;
    float lz = wz - config_.origin.z;

    tile::Cell c;
    if (grid_->topology() == tile::Topology::Hex) {
        c = pixelToHexCell(lx, lz);
    } else {
        c = tile::Cell{static_cast<int>(std::floor(lx / config_.cellSize)),
                       static_cast<int>(std::floor(lz / config_.cellSize))};
    }
    if (!grid_->inBounds(c)) return false;
    outX = c.x; outY = c.y;
    return true;
}

bool TileWorld::sampleHeight(float wx, float wz, float& outY) const {
    int x = 0, y = 0;
    if (!worldToCell(wx, wz, x, y) || !solid(x, y)) return false;
    outY = config_.origin.y + topY(x, y);
    return true;
}

bool TileWorld::isWalkable(int x, int y, uint32_t blockMask) const {
    if (!solid(x, y)) return false;
    if (blockMask && grid_->hasFlag({x, y}, blockMask)) return false;
    return true;
}

TileWorld::CellRayHit TileWorld::raycastCell(const bromath::Vec3& origin,
                                             const bromath::Vec3& dir,
                                             float maxDist) const {
    CellRayHit out;
    if (!grid_) return out;

    const double cs = config_.cellSize;
    if (cs <= 0.0) return out;

    // Normalise direction so the ray parameter t is in world units.
    double dx = dir.x, dy = dir.y, dz = dir.z;
    const double dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dlen < 1e-12) return out;
    dx /= dlen; dy /= dlen; dz /= dlen;

    // Work in grid-local space (the root node carries config_.origin), so the
    // grid spans local XZ [0 .. W*cs] x [0 .. H*cs] with cell (x,y) at top
    // height elevation*heightStep.
    const double ox = origin.x - config_.origin.x;
    const double oy = origin.y - config_.origin.y;
    const double oz = origin.z - config_.origin.z;

    const int W = config_.width, H = config_.height;
    const double eps = 1e-6;

    // Clip the ray to the grid's XZ bounding box so a distant camera still
    // resolves into the grid in a bounded number of DDA steps.
    double tBox0 = 0.0, tBox1 = maxDist;
    auto slab = [&](double o_, double d_, double lo, double hi) -> bool {
        if (std::fabs(d_) < 1e-12) return o_ >= lo && o_ <= hi;
        double ta = (lo - o_) / d_, tb = (hi - o_) / d_;
        if (ta > tb) std::swap(ta, tb);
        tBox0 = std::max(tBox0, ta);
        tBox1 = std::min(tBox1, tb);
        return tBox1 >= tBox0;
    };

    // Hex has no clean analytic DDA (neighbour steps aren't axis-aligned), so this
    // path marches in small steps and bisects at cell-boundary crossings to localize
    // them — precision is bounded by dt below (refined ~2^14x by the bisection),
    // not exact like square's slab DDA. Good enough for interactive picking.
    if (grid_->topology() == tile::Topology::Hex) {
        if (W <= 0 || H <= 0) return out;

        double minX = std::numeric_limits<double>::infinity(), maxX = -minX;
        double minZ = minX, maxZ = -minX;
        auto sweepHex = [&](int x, int y) {
            float ccx = 0, ccz = 0;
            cellCenterLocal(x, y, ccx, ccz);
            for (int i = 0; i < 6; ++i) {
                double px = ccx + cs * kHexCorner[i].x;
                double pz = ccz + cs * kHexCorner[i].z;
                minX = std::min(minX, px); maxX = std::max(maxX, px);
                minZ = std::min(minZ, pz); maxZ = std::max(maxZ, pz);
            }
        };
        for (int x = 0; x < W; ++x) { sweepHex(x, 0); sweepHex(x, H - 1); }
        for (int y = 0; y < H; ++y) { sweepHex(0, y); sweepHex(W - 1, y); }

        if (!slab(ox, dx, minX, maxX)) return out;
        if (!slab(oz, dz, minZ, maxZ)) return out;
        if (tBox0 > tBox1) return out;

        const double tEnd = std::min<double>(maxDist, tBox1);
        double t = std::max(0.0, tBox0);
        if (t > tEnd) return out;
        const double dt = cs / 8.0;

        auto cellAt = [&](double tt) {
            return pixelToHexCell(static_cast<float>(ox + dx * tt),
                                  static_cast<float>(oz + dz * tt));
        };

        tile::Cell cur = cellAt(t + eps);
        const int cap = static_cast<int>((tEnd - t) / std::max(dt, 1e-9)) + 64;

        for (int iter = 0; iter < cap && t <= tEnd + eps; ++iter) {
            // Find this cell's exit time by marching forward until the cell changes,
            // then bisecting the last step to localize the crossing precisely.
            double probe = t;
            tile::Cell nextCell = cur;
            while (probe < tEnd) {
                double p2 = std::min(probe + dt, tEnd);
                tile::Cell c2 = cellAt(p2);
                if (c2 != cur) {
                    double lo = probe, hi = p2;
                    for (int b = 0; b < 14; ++b) {
                        double mid = (lo + hi) * 0.5;
                        if (cellAt(mid) == cur) lo = mid; else hi = mid;
                    }
                    probe = lo;
                    nextCell = c2;
                    break;
                }
                probe = p2;
                if (probe >= tEnd) { nextCell = cur; break; }
            }
            const double tExit = probe;

            if (grid_->inBounds(cur) && solid(cur.x, cur.y)) {
                const double top = static_cast<double>(grid_->elevation(cur)) * config_.heightStep;
                const double yEnter = oy + dy * t;
                if (dy < 0.0 && yEnter <= top + eps && t <= maxDist) {
                    const double th = std::max(t, 0.0);
                    out.hit = true; out.x = cur.x; out.y = cur.y; out.side = true;
                    out.distance = static_cast<float>(th);
                    out.point[0] = static_cast<float>(origin.x + dx * th);
                    out.point[1] = static_cast<float>(origin.y + dy * th);
                    out.point[2] = static_cast<float>(origin.z + dz * th);
                    return out;
                }
                if (std::fabs(dy) > 1e-12) {
                    const double th = (top - oy) / dy;
                    if (th >= t - eps && th <= tExit + eps && th >= 0.0 && th <= maxDist) {
                        out.hit = true; out.x = cur.x; out.y = cur.y; out.side = false;
                        out.distance = static_cast<float>(th);
                        out.point[0] = static_cast<float>(origin.x + dx * th);
                        out.point[1] = static_cast<float>(origin.y + dy * th);
                        out.point[2] = static_cast<float>(origin.z + dz * th);
                        return out;
                    }
                }
            }

            if (tExit >= tEnd - eps || nextCell == cur) break;
            t = tExit;
            cur = nextCell;
        }
        return out;
    }

    if (!slab(ox, dx, 0.0, W * cs)) return out;
    if (!slab(oz, dz, 0.0, H * cs)) return out;
    if (tBox0 > tBox1) return out;

    double tEnter = std::max(0.0, tBox0);
    // Entry point, nudged a touch inward so we start inside the box.
    double ex = ox + dx * (tEnter + eps);
    double ez = oz + dz * (tEnter + eps);
    int ix = static_cast<int>(std::floor(ex / cs));
    int iz = static_cast<int>(std::floor(ez / cs));
    ix = std::min(std::max(ix, 0), W - 1);
    iz = std::min(std::max(iz, 0), H - 1);

    const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
    const int stepZ = dz > 0 ? 1 : (dz < 0 ? -1 : 0);

    auto axis = [&](int i, int step, double o_, double d_,
                    double& tMax, double& tDelta) {
        if (step == 0) { tMax = 1e300; tDelta = 1e300; return; }
        double next = static_cast<double>(i + (step > 0 ? 1 : 0)) * cs;
        tMax   = (next - o_) / d_;
        tDelta = cs / std::fabs(d_);
    };
    double tMaxX, tDeltaX, tMaxZ, tDeltaZ;
    axis(ix, stepX, ox, dx, tMaxX, tDeltaX);
    axis(iz, stepZ, oz, dz, tMaxZ, tDeltaZ);

    const int cap = (W + H) * 2 + 16;
    for (int iter = 0; iter < cap; ++iter) {
        const double tExit = std::min(tMaxX, tMaxZ);

        if (ix >= 0 && iz >= 0 && ix < W && iz < H && solid(ix, iz)) {
            const double top =
                static_cast<double>(grid_->elevation({ix, iz})) * config_.heightStep;
            const double yEnter = oy + dy * tEnter;
            if (dy < 0.0 && yEnter <= top + eps && tEnter <= maxDist) {
                // The descending ray entered this column already at/below the
                // top surface — it struck the cliff face (or grazes the edge).
                const double th = std::max(tEnter, 0.0);
                out.hit = true; out.x = ix; out.y = iz; out.side = true;
                out.distance = static_cast<float>(th);
                out.point[0] = static_cast<float>(origin.x + dx * th);
                out.point[1] = static_cast<float>(origin.y + dy * th);
                out.point[2] = static_cast<float>(origin.z + dz * th);
                return out;
            }
            if (std::fabs(dy) > 1e-12) {
                const double th = (top - oy) / dy;
                if (th >= tEnter - eps && th <= tExit + eps &&
                    th >= 0.0 && th <= maxDist) {
                    out.hit = true; out.x = ix; out.y = iz; out.side = false;
                    out.distance = static_cast<float>(th);
                    out.point[0] = static_cast<float>(origin.x + dx * th);
                    out.point[1] = static_cast<float>(origin.y + dy * th);
                    out.point[2] = static_cast<float>(origin.z + dz * th);
                    return out;
                }
            }
        }

        if (tExit > std::min<double>(maxDist, tBox1)) break;
        if (stepX == 0 && stepZ == 0) break;   // vertical ray, single cell
        if (tMaxX < tMaxZ) { ix += stepX; tEnter = tMaxX; tMaxX += tDeltaX; }
        else               { iz += stepZ; tEnter = tMaxZ; tMaxZ += tDeltaZ; }
    }
    return out;
}

void TileWorld::setOrigin(float x, float y, float z) {
    config_.origin = {x, y, z};
    if (root_) root_->setPosition(config_.origin);
}

// ---- meshing ------------------------------------------------------------

void TileWorld::buildChunkMesh(int ccx, int ccy) {
    const int idx = chunkIdx(ccx, ccy);
    Chunk& chunk = chunks_[idx];
    chunk.dirty = false;

    buildGroundMesh(ccx, ccy, chunk);

    // Overlay layers (>= 1) render as decal meshes above the ground. Resize the
    // per-chunk overlay slot list to match the layer count.
    const int overlayLayers = grid_ ? std::max(0, grid_->layerCount() - 1) : 0;
    if (static_cast<int>(chunk.overlays.size()) != overlayLayers) {
        for (auto* ov : chunk.overlays) if (ov) graph_.destroyNode(ov);
        chunk.overlays.assign(overlayLayers, nullptr);
    }
    for (int L = 1; L <= overlayLayers; ++L)
        buildOverlayMesh(ccx, ccy, chunk, L);

    // Note whether this chunk holds any animated tile, so advance() only
    // remeshes the chunks that actually animate.
    chunkAnimated_[idx] = 0;
    if (grid_ && !config_.animations.empty()) {
        const int cz = config_.chunkSize;
        const int x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
        const int y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);
        const int layers = grid_->layerCount();
        for (int y = y0; y < y1 && !chunkAnimated_[idx]; ++y)
            for (int x = x0; x < x1 && !chunkAnimated_[idx]; ++x)
                for (int L = 0; L < layers; ++L)
                    if (cellIsAnimated(grid_->tile(L, {x, y}))) { chunkAnimated_[idx] = 1; break; }
    }
}

bool TileWorld::advance(double dtMs) {
    if (config_.animations.empty()) return false;
    animClock_ += dtMs;
    bool changed = false;
    for (size_t i = 0; i < config_.animations.size(); ++i) {
        const auto& a = config_.animations[i];
        int n = static_cast<int>(a.frames.size());
        if (n <= 0 || a.fps <= 0.0f) continue;
        int nf = static_cast<int>(animClock_ * (a.fps / 1000.0)) % n;
        if (nf < 0) nf += n;
        if (nf != animFrame_[i]) { animFrame_[i] = nf; changed = true; }
    }
    if (!changed) return false;
    for (int idx = 0; idx < static_cast<int>(chunks_.size()); ++idx) {
        if (idx < static_cast<int>(chunkAnimated_.size()) && chunkAnimated_[idx])
            buildChunkMesh(idx % chunksX_, idx / chunksX_);
    }
    return true;
}

void TileWorld::buildGroundMesh(int ccx, int ccy, Chunk& chunk) {
    const float cs = config_.cellSize;
    const int   cz = config_.chunkSize;
    const int   x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
    const int   y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);
    const bool  hex = grid_ && grid_->topology() == tile::Topology::Hex;

    // Chunk-local origin so baked coords stay small; the node carries the offset.
    // Hex has no clean "corner" reference the way square does, so use the first
    // cell's own pixel center — any fixed reference works since cellCenterLocal is
    // affine and everything below subtracts the same (ox, oz).
    float ox = static_cast<float>(x0) * cs;
    float oz = static_cast<float>(y0) * cs;
    if (hex) cellCenterLocal(x0, y0, ox, oz);

    const float skirtY = static_cast<float>(config_.baseLevel) * config_.heightStep;
    const bool atlas = hasAtlas();

    auto paletteColor = [&](uint16_t id, float out[3]) {
        // With an atlas the texture carries hue, so the surface is white and AO
        // shading rides on the per-vertex colour.
        if (atlas) { out[0] = out[1] = out[2] = 1.0f; return; }
        int n = static_cast<int>(config_.palette.size() / 4);
        if (id < n) {
            out[0] = config_.palette[id * 4 + 0];
            out[1] = config_.palette[id * 4 + 1];
            out[2] = config_.palette[id * 4 + 2];
        } else {
            out[0] = out[1] = out[2] = 0.6f;
        }
    };

    bromesh::MeshData mesh;

    if (hex) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (!solid(x, y)) continue;

                const uint16_t groundId = grid_->tile(0, {x, y});
                const int elev = grid_->elevation({x, y});
                const float hy = static_cast<float>(elev) * config_.heightStep;

                float base[3];
                paletteColor(groundId, base);

                float tu0 = 0, tu1 = 1, tv0 = 0, tv1 = 1;
                float cu0 = 0, cu1 = 1, cv0 = 0, cv1 = 1;
                if (atlas) {
                    atlasCellRect(atlasLayerCell(x, y, 0, groundId), tu0, tu1, tv0, tv1);
                    int cliff = (config_.cliffCell >= 0) ? config_.cliffCell
                                                         : atlasCellFor(groundId);
                    atlasCellRect(cliff, cu0, cu1, cv0, cv1);
                }

                float tint[4]; cellTint(x, y, tint);
                base[0] *= tint[0]; base[1] *= tint[1]; base[2] *= tint[2];

                float cx = 0, cz = 0;
                cellCenterLocal(x, y, cx, cz);
                cx -= ox; cz -= oz;

                Vec3 corner[6];
                for (int i = 0; i < 6; ++i)
                    corner[i] = Vec3{cx + cs * kHexCorner[i].x, hy, cz + cs * kHexCorner[i].z};

                // Each hex vertex touches exactly 3 cells: this one + the two
                // neighbours bracketing that corner (canonical direction i-1 and i).
                tile::Neighbors nb = tile::neighbors(tile::Topology::Hex, {x, y});
                auto higher = [&](int dir) {
                    tile::Cell c = nb[dir];
                    return solid(c.x, c.y) && grid_->elevation(c) > elev;
                };
                float col[6][4];
                float uv[12];
                for (int i = 0; i < 6; ++i) {
                    int occ = (higher((i + 5) % 6) ? 1 : 0) + (higher(i) ? 1 : 0);
                    float s = 1.0f - config_.aoStrength * (static_cast<float>(occ) / 2.0f);
                    col[i][0] = base[0] * s; col[i][1] = base[1] * s;
                    col[i][2] = base[2] * s; col[i][3] = 1.0f;
                    if (atlas) {
                        // Inscribe the hex in its atlas cell's square rect.
                        uv[i * 2 + 0] = tu0 + (tu1 - tu0) * (kHexCorner[i].x / 0.8660254f + 1.0f) * 0.5f;
                        uv[i * 2 + 1] = tv0 + (tv1 - tv0) * (kHexCorner[i].z + 1.0f) * 0.5f;
                    }
                }
                const float* colp[6] = {col[0], col[1], col[2], col[3], col[4], col[5]};
                addFan(mesh, corner, colp, atlas ? uv : nullptr, 6, {0, 1, 0});

                const float sideShade = 0.72f;
                float side[4] = {base[0] * sideShade, base[1] * sideShade, base[2] * sideShade, 1.0f};
                const float cliffUV[8] = {cu0, cv1, cu0, cv0, cu1, cv0, cu1, cv1};
                const float* cliffUVp = atlas ? cliffUV : nullptr;

                for (int i = 0; i < 6; ++i) {
                    tile::Cell nc = nb[i];
                    float ny = solid(nc.x, nc.y) ? topY(nc.x, nc.y) : skirtY;
                    if (ny >= hy) continue;
                    const Vec3& a = corner[i];
                    const Vec3& b = corner[(i + 1) % 6];
                    Vec3 nrm{kHexDir[i].x, 0.0f, kHexDir[i].z};
                    addQuad(mesh, {a.x, ny, a.z}, {a.x, hy, a.z}, {b.x, hy, b.z}, {b.x, ny, b.z},
                            nrm, side, side, side, side, cliffUVp);
                }
            }
        }
    } else {
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!solid(x, y)) continue;

            const uint16_t groundId = grid_->tile(0, {x, y});
            const int elev = grid_->elevation({x, y});
            const float hy = static_cast<float>(elev) * config_.heightStep;

            float base[3];
            paletteColor(groundId, base);

            // Atlas UVs for this cell's top face and its cliff faces.
            float tu0 = 0, tu1 = 1, tv0 = 0, tv1 = 1;   // top cell rect
            float cu0 = 0, cu1 = 1, cv0 = 0, cv1 = 1;   // cliff cell rect
            if (atlas) {
                atlasCellRect(atlasLayerCell(x, y, 0, groundId), tu0, tu1, tv0, tv1);
                int cliff = (config_.cliffCell >= 0) ? config_.cliffCell
                                                     : atlasCellFor(groundId);
                atlasCellRect(cliff, cu0, cu1, cv0, cv1);
            }

            // Per-cell RGB tint multiplies the ground colour (alpha unused on
            // the opaque ground layer).
            float tint[4]; cellTint(x, y, tint);
            base[0] *= tint[0]; base[1] *= tint[1]; base[2] *= tint[2];
            // Top-face corner UVs (order matches the addQuad corners below:
            // a=(x0,z0), b=(x0,z1), c=(x1,z1), d=(x1,z0)).
            const float topUV[8] = {tu0, tv0, tu0, tv1, tu1, tv1, tu1, tv0};
            const float* topUVp = atlas ? topUV : nullptr;

            // Local-space cell rectangle corners on the XZ plane.
            const float lx0 = static_cast<float>(x) * cs - ox;
            const float lx1 = lx0 + cs;
            const float lz0 = static_cast<float>(y) * cs - oz;
            const float lz1 = lz0 + cs;

            // --- top face, with per-corner ambient occlusion ---
            // Corner -> the three neighbour cells that touch it (edge, edge, diag).
            auto cornerAO = [&](int ex, int ez) -> float {
                // ex/ez in {-1,+1} pick the corner quadrant.
                int occ = 0;
                auto higher = [&](int nx, int ny) {
                    return solid(nx, ny) && grid_->elevation({nx, ny}) > elev;
                };
                if (higher(x + ex, y))      ++occ;
                if (higher(x,      y + ez)) ++occ;
                if (higher(x + ex, y + ez)) ++occ;
                return 1.0f - config_.aoStrength * (static_cast<float>(occ) / 3.0f);
            };
            float s00 = cornerAO(-1, -1);  // (lx0,lz0)
            float s10 = cornerAO(+1, -1);  // (lx1,lz0)
            float s11 = cornerAO(+1, +1);  // (lx1,lz1)
            float s01 = cornerAO(-1, +1);  // (lx0,lz1)

            float c00[4] = {base[0]*s00, base[1]*s00, base[2]*s00, 1.0f};
            float c10[4] = {base[0]*s10, base[1]*s10, base[2]*s10, 1.0f};
            float c11[4] = {base[0]*s11, base[1]*s11, base[2]*s11, 1.0f};
            float c01[4] = {base[0]*s01, base[1]*s01, base[2]*s01, 1.0f};

            addQuad(mesh,
                    {lx0, hy, lz0}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, hy, lz0},
                    {0, 1, 0}, c00, c01, c11, c10, topUVp);

            // --- cliff faces on edges that drop to a lower neighbour / skirt ---
            const float sideShade = 0.72f;
            float side[4] = {base[0]*sideShade, base[1]*sideShade, base[2]*sideShade, 1.0f};

            // Cliff corner UVs (each cliff is wound bottom,top,top,bottom going
            // left→right, so one rect mapping serves all four edges).
            const float cliffUV[8] = {cu0, cv1, cu0, cv0, cu1, cv0, cu1, cv1};
            const float* cliffUVp = atlas ? cliffUV : nullptr;

            auto neighbourTopY = [&](int nx, int ny) -> float {
                return solid(nx, ny) ? topY(nx, ny) : skirtY;
            };

            // East (+X) edge at lx1, spanning lz0..lz1, faces +X.
            if (float ny = neighbourTopY(x + 1, y); ny < hy)
                addQuad(mesh, {lx1, ny, lz0}, {lx1, hy, lz0}, {lx1, hy, lz1}, {lx1, ny, lz1},
                        {1, 0, 0}, side, side, side, side, cliffUVp);
            // West (-X) edge at lx0, faces -X.
            if (float ny = neighbourTopY(x - 1, y); ny < hy)
                addQuad(mesh, {lx0, ny, lz0}, {lx0, hy, lz0}, {lx0, hy, lz1}, {lx0, ny, lz1},
                        {-1, 0, 0}, side, side, side, side, cliffUVp);
            // South (+Z) edge at lz1, faces +Z.
            if (float ny = neighbourTopY(x, y + 1); ny < hy)
                addQuad(mesh, {lx0, ny, lz1}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, ny, lz1},
                        {0, 0, 1}, side, side, side, side, cliffUVp);
            // North (-Z) edge at lz0, faces -Z.
            if (float ny = neighbourTopY(x, y - 1); ny < hy)
                addQuad(mesh, {lx0, ny, lz0}, {lx0, hy, lz0}, {lx1, hy, lz0}, {lx1, ny, lz0},
                        {0, 0, -1}, side, side, side, side, cliffUVp);
        }
    }
    }

    if (mesh.empty()) {
        // Nothing solid in this chunk — drop any stale node.
        if (chunk.ground) { graph_.destroyNode(chunk.ground); chunk.ground = nullptr; }
        return;
    }

    if (!chunk.ground) {
        chunk.ground = graph_.createMesh("tile-chunk");
        root_->addChild(chunk.ground);
        chunk.ground->setColor(1, 1, 1, 1);
        chunk.ground->setRoughness(0.92f);
        chunk.ground->setMetallic(0.0f);
        if (atlas)
            chunk.ground->setBaseColorTexture(config_.atlasWidth, config_.atlasHeight,
                                              config_.atlasPixels.data());
    }
    chunk.ground->setMesh(std::move(mesh));
    chunk.ground->setPosition(ox, 0.0f, oz);
}

// Build the decal mesh for one overlay layer of a chunk. Each cell with a
// non-empty tile on `layer` emits a single up-facing quad floating just above
// the ground top face, atlas-textured by the layer tile id (autotile-aware),
// tinted per cell. The whole layer carries its style's opacity + alphaCutoff.
void TileWorld::buildOverlayMesh(int ccx, int ccy, Chunk& chunk, int layer) {
    MeshNode*& node = chunk.overlays[layer - 1];

    const bool atlas = hasAtlas();
    if (!atlas) {                    // overlays are atlas-only
        if (node) { graph_.destroyNode(node); node = nullptr; }
        return;
    }

    const float cs = config_.cellSize;
    const int   cz = config_.chunkSize;
    const int   x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
    const int   y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);
    const bool  hex = grid_->topology() == tile::Topology::Hex;
    float ox = static_cast<float>(x0) * cs;
    float oz = static_cast<float>(y0) * cs;
    if (hex) cellCenterLocal(x0, y0, ox, oz);

    // Small per-layer lift above the ground top so decals don't z-fight; the
    // depth bias below makes it robust regardless of scale.
    const float lift = 0.012f * cs * static_cast<float>(layer);

    bromesh::MeshData mesh;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!solid(x, y)) continue;            // overlays ride on solid ground
            const uint16_t id = grid_->tile(layer, {x, y});
            if (id == 0) continue;

            const float hy = topY(x, y) + lift;

            float u0, u1, v0, v1;
            atlasCellRect(atlasLayerCell(x, y, layer, id), u0, u1, v0, v1);

            float t[4]; cellTint(x, y, t);
            float col[4] = {t[0], t[1], t[2], t[3]};

            if (hex) {
                float cx = 0, cz2 = 0;
                cellCenterLocal(x, y, cx, cz2);
                cx -= ox; cz2 -= oz;
                Vec3 corner[6];
                float uv[12];
                const float* colp[6];
                for (int i = 0; i < 6; ++i) {
                    corner[i] = Vec3{cx + cs * kHexCorner[i].x, hy, cz2 + cs * kHexCorner[i].z};
                    uv[i * 2 + 0] = u0 + (u1 - u0) * (kHexCorner[i].x / 0.8660254f + 1.0f) * 0.5f;
                    uv[i * 2 + 1] = v0 + (v1 - v0) * (kHexCorner[i].z + 1.0f) * 0.5f;
                    colp[i] = col;
                }
                addFan(mesh, corner, colp, uv, 6, {0, 1, 0});
                continue;
            }

            const float lx0 = static_cast<float>(x) * cs - ox;
            const float lx1 = lx0 + cs;
            const float lz0 = static_cast<float>(y) * cs - oz;
            const float lz1 = lz0 + cs;
            const float uv[8] = {u0, v0, u0, v1, u1, v1, u1, v0};
            addQuad(mesh,
                    {lx0, hy, lz0}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, hy, lz0},
                    {0, 1, 0}, col, col, col, col, uv);
        }
    }

    if (mesh.empty()) {
        if (node) { graph_.destroyNode(node); node = nullptr; }
        return;
    }

    TileWorldConfig::OverlayStyle style;
    if (layer < static_cast<int>(config_.overlays.size()))
        style = config_.overlays[layer];

    if (!node) {
        node = graph_.createMesh("tile-overlay");
        root_->addChild(node);
        node->setRoughness(0.95f);
        node->setMetallic(0.0f);
        node->setDepthBias(-1.0f, -static_cast<float>(layer) - 1.0f);
        node->setCastsShadow(false);
        node->setBaseColorTexture(config_.atlasWidth, config_.atlasHeight,
                                  config_.atlasPixels.data());
    }
    node->setColor(1, 1, 1, style.opacity);
    node->setAlphaCutoff(style.alphaCutoff);
    node->setMesh(std::move(mesh));
    node->setPosition(ox, 0.0f, oz);
}

void TileWorld::rebuildDirty() {
    for (int ccy = 0; ccy < chunksY_; ++ccy)
        for (int ccx = 0; ccx < chunksX_; ++ccx)
            if (chunks_[chunkIdx(ccx, ccy)].dirty)
                buildChunkMesh(ccx, ccy);
    rebuildObjects();
}

void TileWorld::rebuildAll() {
    for (int ccy = 0; ccy < chunksY_; ++ccy)
        for (int ccx = 0; ccx < chunksX_; ++ccx)
            buildChunkMesh(ccx, ccy);
    rebuildObjects();
}

// ---- objects ------------------------------------------------------------

int TileWorld::addObjectKind(bromesh::MeshData&& mesh, const ObjectStyle& style) {
    if (!root_ || mesh.empty()) return -1;

    auto* node = graph_.createInstancedMesh("tile-objects");
    root_->addChild(node);
    node->setMesh(std::move(mesh));
    node->setColor(style.color[0], style.color[1], style.color[2], style.color[3]);
    node->setRoughness(style.roughness);
    node->setMetallic(style.metallic);
    node->setDoubleSided(style.doubleSided);
    node->setAlphaCutoff(style.alphaCutoff);
    node->setCastsShadow(style.castsShadow);
    node->setAtlasGrid(style.atlasCols, style.atlasRows);
    if (!style.texPixels.empty() && style.texWidth > 0 && style.texHeight > 0)
        node->setBaseColorTexture(style.texWidth, style.texHeight, style.texPixels.data());

    objectKinds_.push_back(ObjectKind{});
    objectKinds_.back().node = node;
    return static_cast<int>(objectKinds_.size()) - 1;
}

int TileWorld::addObject(int kind, int x, int y, const ObjectPlacement& p) {
    if (kind < 0 || kind >= static_cast<int>(objectKinds_.size())) return -1;
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return -1;
    ObjectKind& k = objectKinds_[kind];
    k.placements.push_back(p);
    k.cellX.push_back(x);
    k.cellY.push_back(y);
    k.dirty = true;
    return static_cast<int>(k.placements.size()) - 1;
}

void TileWorld::clearObjects(int kind) {
    auto clearOne = [](ObjectKind& k) {
        k.placements.clear(); k.cellX.clear(); k.cellY.clear(); k.dirty = true;
    };
    if (kind < 0) { for (auto& k : objectKinds_) clearOne(k); return; }
    if (kind < static_cast<int>(objectKinds_.size())) clearOne(objectKinds_[kind]);
}

int TileWorld::objectCount(int kind) const {
    if (kind < 0 || kind >= static_cast<int>(objectKinds_.size())) return 0;
    return static_cast<int>(objectKinds_[kind].placements.size());
}

void TileWorld::rebuildObjectKind(ObjectKind& k) {
    k.dirty = false;
    if (!k.node) return;

    const size_t n = k.placements.size();
    const float cs = config_.cellSize;

    std::vector<float> inst(n * 16);
    for (size_t i = 0; i < n; ++i) {
        const ObjectPlacement& p = k.placements[i];
        const int x = k.cellX[i], y = k.cellY[i];

        // Cell-centre anchor in root-local space (root carries the origin).
        float px = 0, pz = 0;
        cellCenterLocal(x, y, px, pz);
        px += p.offsetX * cs;
        pz += p.offsetZ * cs;
        const float py = topY(x, y) + p.yOffset;

        const float s = p.scale;
        const float c = std::cos(p.yaw), sn = std::sin(p.yaw);

        float* o = inst.data() + i * 16;
        // Y-rotation * uniform scale (row-major 4x3; rows 0..2 carry the affine).
        o[ 0] = c * s;  o[ 1] = 0;  o[ 2] = sn * s;  o[ 3] = px;
        o[ 4] = 0;      o[ 5] = s;  o[ 6] = 0;       o[ 7] = py;
        o[ 8] = -sn * s; o[9] = 0;  o[10] = c * s;   o[11] = pz;
        // Per-instance tint; when the kind has an atlas, alpha carries the
        // variant cell index (matches setInstancesFromPosQuatScale's packing).
        o[12] = p.color[0]; o[13] = p.color[1]; o[14] = p.color[2];
        bool atlas = (k.node->atlasCols() > 1 || k.node->atlasRows() > 1);
        if (atlas) {
            float vi = static_cast<float>(p.variant);
            vi = vi < 0.0f ? 0.0f : (vi > 255.0f ? 255.0f : vi);
            o[15] = (vi + 0.5f) / 256.0f;
        } else {
            o[15] = p.color[3];
        }
    }
    k.node->setInstances(inst.data(), n);
}

void TileWorld::rebuildObjects() {
    for (auto& k : objectKinds_)
        if (k.dirty) rebuildObjectKind(k);
}

// ---- stats --------------------------------------------------------------

int TileWorld::chunkCount() const {
    int n = 0;
    for (auto& c : chunks_) if (c.ground) ++n;
    return n;
}

int TileWorld::totalVertices() const {
    int n = 0;
    for (auto& c : chunks_) {
        if (c.ground) n += static_cast<int>(c.ground->mesh().vertexCount());
        for (auto* ov : c.overlays)
            if (ov) n += static_cast<int>(ov->mesh().vertexCount());
    }
    return n;
}

int TileWorld::totalTriangles() const {
    int n = 0;
    for (auto& c : chunks_) {
        if (c.ground) n += static_cast<int>(c.ground->mesh().triangleCount());
        for (auto* ov : c.overlays)
            if (ov) n += static_cast<int>(ov->mesh().triangleCount());
    }
    return n;
}

} // namespace bro::scene
