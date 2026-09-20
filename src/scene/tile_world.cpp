#include "scene/tile_world.h"

#include "scene/scene_graph.h"
#include "scene/gl_available.h"
#include "scene/mesh_node.h"
#include "scene/instanced_mesh_node.h"
#include "scene/scene_node.h"

#include "tile/autotile.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "scene/tile_world_internal.h"

namespace bro::scene {

using bromath::Vec3;

namespace {
constexpr float kHexSqrt3 = 1.7320508075688772f;
}  // namespace

// -------------------------------------------------------------------------
// TileWorld
// -------------------------------------------------------------------------

TileWorld::TileWorld(SceneGraph& graph) : graphToken_(graph.livenessToken()) {}

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
            float px = cx + R * hexCorners()[i].x;
            float pz = cz + R * hexCorners()[i].z;
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
    // Registered object kinds are rendering config (like the atlas/palette) and
    // survive a load: detach their nodes so clear() doesn't destroy them, keep
    // placements that fit within the new grid bounds, and re-attach under the
    // fresh root afterwards. Kind ids and surviving placements stay valid.
    const int newW = newGrid.width();
    const int newH = newGrid.height();
    std::vector<ObjectKind> kinds = std::move(objectKinds_);
    objectKinds_.clear();
    for (auto& k : kinds) {
        if (k.node) k.node->removeFromParent();
        std::vector<ObjectPlacement> keptPlacements;
        std::vector<int> keptX;
        std::vector<int> keptY;
        for (size_t i = 0; i < k.placements.size(); ++i) {
            int cx = k.cellX[i];
            int cy = k.cellY[i];
            if (cx >= 0 && cx < newW && cy >= 0 && cy < newH) {
                keptPlacements.push_back(k.placements[i]);
                keptX.push_back(cx);
                keptY.push_back(cy);
            }
        }
        k.placements = std::move(keptPlacements);
        k.cellX = std::move(keptX);
        k.cellY = std::move(keptY);
        k.dirty = true;
    }

    clear();
    config_.width     = newGrid.width();
    config_.height    = newGrid.height();
    config_.topology  = newGrid.topology();
    config_.layers    = newGrid.layerNames();
    config_.chunkSize = std::max(1, config_.chunkSize);

    grid_ = std::make_unique<tile::TileGrid>(std::move(newGrid));
    objectKinds_ = std::move(kinds);
    initFromGrid();
    // rootNode() is null when the graph is gone — initFromGrid() built nothing
    // and every k.node is a pointer the reclaimed graph already freed.
    if (auto* r = rootNode())
        for (auto& k : objectKinds_)
            if (k.node) r->addChild(k.node);
    rebuildObjects();
}

void TileWorld::initFromGrid() {
    chunksX_ = (config_.width  + config_.chunkSize - 1) / config_.chunkSize;
    chunksY_ = (config_.height + config_.chunkSize - 1) / config_.chunkSize;
    chunks_.assign(static_cast<size_t>(chunksX_) * chunksY_, Chunk{});
    chunkAnimated_.assign(chunks_.size(), 0);

    tint_.assign(static_cast<size_t>(config_.width) * config_.height, 0xFFFFFFFFu);
    shade_.assign(static_cast<size_t>(config_.width) * config_.height, 255);
    shadeDirtyY0_ = 0;
    shadeDirtyY1_ = -1;

    // Build the tile-id -> animation index map and reset frame state.
    animClock_ = 0.0;
    animFrame_.assign(config_.animations.size(), 0);
    animOf_.clear();
    for (size_t i = 0; i < config_.animations.size(); ++i) {
        uint16_t id = config_.animations[i].id;
        if (id >= animOf_.size()) animOf_.resize(id + 1, -1);
        animOf_[id] = static_cast<int>(i);
    }

    auto* g = graph();
    if (!g) return;   // graph destroyed under us — the grid stands, unmeshed
    root_ = g->createNode("tileworld");
    g->root()->addChild(root_);
    root_->setPosition(config_.origin);

    rebuildAll();
}

void TileWorld::clear() {
    // A dead graph already destroyed every node it owned; the pointers below
    // are the freed ones. Drop them without dereferencing. This is the path
    // ~TileWorld takes when a JS handle outlives its canvas.
    if (auto* g = graph()) {
        for (auto& c : chunks_) {
            if (c.ground) g->destroyNode(c.ground);
            for (auto* ov : c.overlays) if (ov) g->destroyNode(ov);
        }
        for (auto& k : objectKinds_)
            if (k.node) g->destroyNode(k.node);
        if (root_) g->destroyNode(root_);
        releaseShadeTexture();
    }
    shadeTex_ = 0;
    shadeUsed_ = false;
    shade_.clear();
    chunks_.clear();
    objectKinds_.clear();
    root_ = nullptr;
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

uint32_t TileWorld::tintAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return 0xFFFFFFFFu;
    size_t idx = static_cast<size_t>(y) * config_.width + x;
    return idx < tint_.size() ? tint_[idx] : 0xFFFFFFFFu;
}

// ---- shade map ----------------------------------------------------------

static uint8_t quantizeShade(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

void TileWorld::setShade(int x, int y, float v) {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return;
    size_t idx = static_cast<size_t>(y) * config_.width + x;
    if (idx >= shade_.size()) return;
    const uint8_t q = quantizeShade(v);
    shadeUsed_ = true;
    if (shade_[idx] == q) return;
    shade_[idx] = q;
    markShadeDirty(y);
}

void TileWorld::fillShade(int x0, int y0, int x1, int y1, float v) {
    const uint8_t q = quantizeShade(v);
    int lo_x = std::max(0, std::min(x0, x1)), hi_x = std::min(config_.width - 1, std::max(x0, x1));
    int lo_y = std::max(0, std::min(y0, y1)), hi_y = std::min(config_.height - 1, std::max(y0, y1));
    if (lo_x > hi_x || lo_y > hi_y || shade_.empty()) return;
    shadeUsed_ = true;
    for (int y = lo_y; y <= hi_y; ++y) {
        bool rowChanged = false;
        for (int x = lo_x; x <= hi_x; ++x) {
            uint8_t& cell = shade_[static_cast<size_t>(y) * config_.width + x];
            if (cell == q) continue;
            cell = q;
            rowChanged = true;
        }
        if (rowChanged) markShadeDirty(y);
    }
}

void TileWorld::setShadeMap(const float* values, size_t count) {
    if (!values || shade_.empty()) return;
    shadeUsed_ = true;
    const size_t n = std::min(count, shade_.size());
    for (size_t i = 0; i < n; ++i) {
        const uint8_t q = quantizeShade(values[i]);
        if (shade_[i] == q) continue;
        shade_[i] = q;
        markShadeDirty(static_cast<int>(i / config_.width));
    }
}

void TileWorld::setShadeMap(const uint8_t* values, size_t count) {
    if (!values || shade_.empty()) return;
    shadeUsed_ = true;
    const size_t n = std::min(count, shade_.size());
    for (size_t i = 0; i < n; ++i) {
        if (shade_[i] == values[i]) continue;
        shade_[i] = values[i];
        markShadeDirty(static_cast<int>(i / config_.width));
    }
}

float TileWorld::shadeAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= config_.width || y >= config_.height) return 1.0f;
    size_t idx = static_cast<size_t>(y) * config_.width + x;
    return idx < shade_.size() ? shade_[idx] / 255.0f : 1.0f;
}

bool TileWorld::shadeBinding(ShadeMapBinding& out) {
    if (!shadeUsed_ || shade_.empty() || !root_) return false;
    if (!glFunctionsLoaded()) return false;
    const int w = config_.width, h = config_.height;
    if (shadeTex_ && (shadeTexW_ != w || shadeTexH_ != h)) releaseShadeTexture();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (!shadeTex_) {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, shade_.data());
        shadeTex_ = tex;
        shadeTexW_ = w;
        shadeTexH_ = h;
        shadeDirtyY1_ = -1;
        shadeDirtyY0_ = 0;
    } else if (shadeDirtyY1_ >= shadeDirtyY0_) {
        const int y0 = std::max(0, shadeDirtyY0_), y1 = std::min(h - 1, shadeDirtyY1_);
        glBindTexture(GL_TEXTURE_2D, shadeTex_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y0, w, y1 - y0 + 1, GL_RED, GL_UNSIGNED_BYTE,
                        shade_.data() + static_cast<size_t>(y0) * w);
        shadeDirtyY1_ = -1;
        shadeDirtyY0_ = 0;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const bromath::Mat4& m = root_->worldMatrix();
    out.tex = shadeTex_;
    out.origin = Vec3{m.at(0, 3), m.at(1, 3), m.at(2, 3)};
    out.cellSize = config_.cellSize;
    out.hex = grid_ && grid_->topology() == tile::Topology::Hex;
    out.width = w;
    out.height = h;
    return true;
}

void TileWorld::attachShadeMap(MeshNode* node) {
    if (node) node->setShadeMap([this](ShadeMapBinding& b) { return shadeBinding(b); });
}

void TileWorld::attachShadeMap(InstancedMeshNode* node) {
    if (node) node->setShadeMap([this](ShadeMapBinding& b) { return shadeBinding(b); });
}

void TileWorld::releaseShadeTexture() {
    if (shadeTex_ && glFunctionsLoaded()) {
        GLuint tex = shadeTex_;
        glDeleteTextures(1, &tex);
    }
    shadeTex_ = 0;
    shadeTexW_ = shadeTexH_ = 0;
    shadeDirtyY1_ = -1;
    shadeDirtyY0_ = 0;
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
    // ANY-bit block: a cell is blocked if it shares any bit with blockMask
    // (same contract as findPath/distanceField's blockMask and passUnlessFlag).
    if ((grid_->flags({x, y}) & blockMask) != 0) return false;
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
                double px = ccx + cs * hexCorners()[i].x;
                double pz = ccz + cs * hexCorners()[i].z;
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
    if (auto* r = rootNode()) r->setPosition(config_.origin);
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
