#include "scene/tile_world.h"

#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "scene/scene_node.h"

#include "tile/autotile.h"

#include <algorithm>
#include <cmath>

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

    chunksX_ = (config_.width  + config_.chunkSize - 1) / config_.chunkSize;
    chunksY_ = (config_.height + config_.chunkSize - 1) / config_.chunkSize;
    chunks_.assign(static_cast<size_t>(chunksX_) * chunksY_, Chunk{});

    tint_.assign(static_cast<size_t>(config_.width) * config_.height, 0xFFFFFFFFu);

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
    if (root_) {
        graph_.destroyNode(root_);
        root_ = nullptr;
    }
    grid_.reset();
    tint_.clear();
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
    float lx = (wx - config_.origin.x) / config_.cellSize;
    float lz = (wz - config_.origin.z) / config_.cellSize;
    int x = static_cast<int>(std::floor(lx));
    int y = static_cast<int>(std::floor(lz));
    if (!grid_->inBounds({x, y})) return false;
    outX = x; outY = y;
    return true;
}

void TileWorld::setOrigin(float x, float y, float z) {
    config_.origin = {x, y, z};
    if (root_) root_->setPosition(config_.origin);
}

// ---- meshing ------------------------------------------------------------

void TileWorld::buildChunkMesh(int ccx, int ccy) {
    Chunk& chunk = chunks_[chunkIdx(ccx, ccy)];
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
}

void TileWorld::buildGroundMesh(int ccx, int ccy, Chunk& chunk) {
    const float cs = config_.cellSize;
    const int   cz = config_.chunkSize;
    const int   x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
    const int   y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);

    // Chunk-local origin so baked coords stay small; the node carries the offset.
    const float ox = static_cast<float>(x0) * cs;
    const float oz = static_cast<float>(y0) * cs;

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
    const float ox = static_cast<float>(x0) * cs;
    const float oz = static_cast<float>(y0) * cs;

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
            const float lx0 = static_cast<float>(x) * cs - ox;
            const float lx1 = lx0 + cs;
            const float lz0 = static_cast<float>(y) * cs - oz;
            const float lz1 = lz0 + cs;

            float u0, u1, v0, v1;
            atlasCellRect(atlasLayerCell(x, y, layer, id), u0, u1, v0, v1);
            const float uv[8] = {u0, v0, u0, v1, u1, v1, u1, v0};

            float t[4]; cellTint(x, y, t);
            float col[4] = {t[0], t[1], t[2], t[3]};
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
}

void TileWorld::rebuildAll() {
    for (int ccy = 0; ccy < chunksY_; ++ccy)
        for (int ccx = 0; ccx < chunksX_; ++ccx)
            buildChunkMesh(ccx, ccy);
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
