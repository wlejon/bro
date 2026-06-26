#include "scene/tile_world.h"

#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "scene/scene_node.h"

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
                    const float cc[4], const float cd[4]) {
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

    root_ = graph_.createNode("tileworld");
    graph_.root()->addChild(root_);
    root_->setPosition(config_.origin);

    rebuildAll();
}

void TileWorld::clear() {
    for (auto& c : chunks_) {
        if (c.node) graph_.destroyNode(c.node);
        c.node = nullptr;
    }
    chunks_.clear();
    if (root_) {
        graph_.destroyNode(root_);
        root_ = nullptr;
    }
    grid_.reset();
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

    const float cs = config_.cellSize;
    const int   cz = config_.chunkSize;
    const int   x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
    const int   y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);

    // Chunk-local origin so baked coords stay small; the node carries the offset.
    const float ox = static_cast<float>(x0) * cs;
    const float oz = static_cast<float>(y0) * cs;

    const float skirtY = static_cast<float>(config_.baseLevel) * config_.heightStep;

    auto paletteColor = [&](uint16_t id, float out[3]) {
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

            const int elev = grid_->elevation({x, y});
            const float hy = static_cast<float>(elev) * config_.heightStep;

            float base[3];
            paletteColor(grid_->tile(0, {x, y}), base);

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
                    {0, 1, 0}, c00, c01, c11, c10);

            // --- cliff faces on edges that drop to a lower neighbour / skirt ---
            const float sideShade = 0.72f;
            float side[4] = {base[0]*sideShade, base[1]*sideShade, base[2]*sideShade, 1.0f};

            auto neighbourTopY = [&](int nx, int ny) -> float {
                return solid(nx, ny) ? topY(nx, ny) : skirtY;
            };

            // East (+X) edge at lx1, spanning lz0..lz1, faces +X.
            if (float ny = neighbourTopY(x + 1, y); ny < hy)
                addQuad(mesh, {lx1, ny, lz0}, {lx1, hy, lz0}, {lx1, hy, lz1}, {lx1, ny, lz1},
                        {1, 0, 0}, side, side, side, side);
            // West (-X) edge at lx0, faces -X.
            if (float ny = neighbourTopY(x - 1, y); ny < hy)
                addQuad(mesh, {lx0, ny, lz0}, {lx0, hy, lz0}, {lx0, hy, lz1}, {lx0, ny, lz1},
                        {-1, 0, 0}, side, side, side, side);
            // South (+Z) edge at lz1, faces +Z.
            if (float ny = neighbourTopY(x, y + 1); ny < hy)
                addQuad(mesh, {lx0, ny, lz1}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, ny, lz1},
                        {0, 0, 1}, side, side, side, side);
            // North (-Z) edge at lz0, faces -Z.
            if (float ny = neighbourTopY(x, y - 1); ny < hy)
                addQuad(mesh, {lx0, ny, lz0}, {lx0, hy, lz0}, {lx1, hy, lz0}, {lx1, ny, lz0},
                        {0, 0, -1}, side, side, side, side);
        }
    }

    if (mesh.empty()) {
        // Nothing solid in this chunk — drop any stale node.
        if (chunk.node) { graph_.destroyNode(chunk.node); chunk.node = nullptr; }
        return;
    }

    if (!chunk.node) {
        chunk.node = graph_.createMesh("tile-chunk");
        root_->addChild(chunk.node);
        chunk.node->setColor(1, 1, 1, 1);
        chunk.node->setRoughness(0.92f);
        chunk.node->setMetallic(0.0f);
    }
    chunk.node->setMesh(std::move(mesh));
    chunk.node->setPosition(ox, 0.0f, oz);
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
    for (auto& c : chunks_) if (c.node) ++n;
    return n;
}

int TileWorld::totalVertices() const {
    int n = 0;
    for (auto& c : chunks_)
        if (c.node) n += static_cast<int>(c.node->mesh().vertexCount());
    return n;
}

int TileWorld::totalTriangles() const {
    int n = 0;
    for (auto& c : chunks_)
        if (c.node) n += static_cast<int>(c.node->mesh().triangleCount());
    return n;
}

} // namespace bro::scene
