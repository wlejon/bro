#pragma once

// tile_world.h — TileWorld: renders a bro::tile::TileGrid as chunked,
// flat-topped tile geometry inside the 3D scene.
//
// This is the scene-side bridge over the pure `bro::tile` grid core. It owns a
// TileGrid (square topology for now; hex designed-in) and meshes it into a set
// of chunk MeshNodes parented under a single root node, so the whole map can be
// positioned/hidden as a unit and any camera (ortho top-down, isometric tilt,
// or perspective) renders it.
//
// Each solid cell (non-empty tile on the ground layer) emits a flat top quad at
// its elevation height, plus vertical CLIFF quads on any edge where the
// neighbour sits lower (or is empty / off-map down to a skirt floor). Top-face
// corners are darkened by an ambient-occlusion term from higher neighbours, so
// valleys and steps read with contact shading. Per-cell colour comes from a
// caller-supplied palette indexed by the ground-layer tile id; tileset-atlas
// texturing layers on top of this in a later step.
//
// Edits mark the touched chunk (and its border neighbours) dirty; rebuildDirty()
// remeshes only those — the same lifecycle TerrainManager uses.

#include "tile/grid.h"
#include "tile/coord.h"

#include <bromath/vec.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::scene {

class SceneGraph;
class SceneNode;
class MeshNode;

// -------------------------------------------------------------------------
// Configuration
// -------------------------------------------------------------------------

struct TileWorldConfig {
    int width  = 16;                 // grid size in cells
    int height = 16;
    tile::Topology topology = tile::Topology::Square;
    std::vector<std::string> layers = {"ground"};  // >= 1; layer 0 is "ground"

    float cellSize   = 1.0f;         // world units per cell along X/Z
    float heightStep = 0.5f;         // world units per elevation level along Y
    int   chunkSize  = 16;           // cells per chunk edge (>= 1)
    int   baseLevel  = 0;            // elevation the map-edge / hole skirt drops to
    float aoStrength = 0.45f;        // 0 = no corner AO, 1 = full darkening

    bromath::Vec3 origin = {0, 0, 0};

    // RGBA per ground-tile id (4 floats each); index 0 = empty (unused for
    // colour). Empty palette -> flat grey.
    std::vector<float> palette;
};

// -------------------------------------------------------------------------
// TileWorld
// -------------------------------------------------------------------------

class TileWorld {
public:
    explicit TileWorld(SceneGraph& graph);
    ~TileWorld();

    TileWorld(const TileWorld&) = delete;
    TileWorld& operator=(const TileWorld&) = delete;

    // Build (or rebuild) the grid + all chunk meshes from `cfg`.
    void configure(const TileWorldConfig& cfg);
    const TileWorldConfig& config() const { return config_; }

    // Direct access to the underlying pure grid (region/autotile/pathfind run
    // off this). Mutating it directly will not mark chunks dirty — prefer the
    // authoring setters below, or call rebuildAll() afterwards.
    tile::TileGrid* grid() { return grid_.get(); }
    const tile::TileGrid* grid() const { return grid_.get(); }

    // ---- authoring (marks affected chunks dirty) ------------------------
    void setTile(int x, int y, uint16_t id, int layer = 0);
    void setElevation(int x, int y, int level);
    void setFlag(int x, int y, uint32_t bit, bool on);
    void fillTile(int x0, int y0, int x1, int y1, uint16_t id, int layer = 0);
    void fillElevation(int x0, int y0, int x1, int y1, int level);

    // ---- query ----------------------------------------------------------
    uint16_t tile(int x, int y, int layer = 0) const;
    int      elevation(int x, int y) const;
    bool     hasFlag(int x, int y, uint32_t bit) const;

    // Map a world XZ position to a grid cell (ignores elevation — a flat
    // top-down projection). Returns false if outside the grid.
    bool worldToCell(float wx, float wz, int& outX, int& outY) const;

    // ---- placement ------------------------------------------------------
    void setOrigin(float x, float y, float z);
    SceneNode* rootNode() const { return root_; }

    // ---- rebuild / teardown --------------------------------------------
    void rebuildDirty();   // remesh only dirty chunks
    void rebuildAll();     // remesh every chunk
    void clear();          // destroy all nodes + grid

    // ---- stats ----------------------------------------------------------
    int width()  const { return config_.width; }
    int height() const { return config_.height; }
    int chunkCount() const;
    int totalVertices() const;
    int totalTriangles() const;

private:
    struct Chunk {
        MeshNode* node = nullptr;   // owned by SceneGraph (nullptr until first mesh)
        bool dirty = false;
    };

    void buildChunkMesh(int ccx, int ccy);
    void markCellDirty(int x, int y);
    int  chunkIdx(int ccx, int ccy) const { return ccy * chunksX_ + ccx; }
    bool solid(int x, int y) const;        // in-bounds + non-empty ground tile
    float topY(int x, int y) const;        // world Y of a cell's top surface

    SceneGraph& graph_;
    TileWorldConfig config_;
    std::unique_ptr<tile::TileGrid> grid_;

    SceneNode* root_ = nullptr;
    int chunksX_ = 0;
    int chunksY_ = 0;
    std::vector<Chunk> chunks_;   // row-major chunksX_ * chunksY_
};

} // namespace bro::scene
