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
// valleys and steps read with contact shading. Surface appearance is either a
// caller-supplied palette indexed by the ground-layer tile id, or — when a
// tileset atlas is configured — per-cell atlas UVs on the top faces (and a
// cliff cell on the sides), with the palette/AO folded into per-vertex shading.
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
    // colour). Empty palette -> flat grey. Ignored when an atlas is set.
    std::vector<float> palette;

    // ---- tileset atlas (optional) ---------------------------------------
    // When atlasPixels is non-empty, top faces sample the atlas cell selected
    // by their ground tile id, cliff faces sample `cliffCell`, and per-vertex
    // colour carries only AO shading (the texture provides hue). The atlas is a
    // regular grid of `atlasColumns` x `atlasRows` cells; cell index counts
    // left-to-right, top-to-bottom. Without an atlas, palette colour is used.
    std::vector<uint8_t> atlasPixels;  // RGBA8, tightly packed, top-left origin
    int atlasWidth   = 0;
    int atlasHeight  = 0;
    int atlasColumns = 1;
    int atlasRows    = 1;
    // Ground tile id -> atlas cell index. Empty => cell index == tile id (so
    // id 1 draws cell 1; cell 0 is unused since id 0 is empty).
    std::vector<int> tileAtlas;
    // Atlas cell for vertical cliff faces. -1 => reuse each column's top cell.
    int cliffCell = -1;
    // UV inset per cell edge, in atlas texels, to fight bilinear/mip bleeding
    // between neighbouring cells. 0 = none.
    float atlasInset = 0.0f;

    // ---- autotiling (optional; requires an atlas) -----------------------
    // An autotile rule turns "a field of the same tile id" into bordered/edge
    // art: a cell's top-face atlas cell is chosen from a neighbour bitmask via
    // a variant table, instead of the flat per-id cell. Each rule applies to one
    // ground tile id. Family = which neighbours "join" (same id, or any
    // non-empty). Cliff faces are unaffected.
    enum class AutotileMode : uint8_t {
        Edge,    // 4-bit edge mask -> 16 variants (E,N,W,S = bits 0..3)
        Blob47,  // 8-neighbour blob reduced to the canonical 47 variants
        Wang,    // 4-bit corner mask -> 16 variants (NE,SE,SW,NW = bits 0..3)
    };
    enum class AutotileFamily : uint8_t { SameId, NonEmpty };
    struct AutotileRule {
        uint16_t id = 0;                 // tile id this rule applies to
        int      layer = 0;              // which layer the rule + family run on
        AutotileMode mode = AutotileMode::Blob47;
        AutotileFamily family = AutotileFamily::SameId;
        std::vector<int> cells;          // variant index -> atlas cell index
    };
    std::vector<AutotileRule> autotiles;

    // ---- overlay layers (optional; require an atlas) --------------------
    // Layers above the ground (index >= 1) render as atlas-textured DECAL quads
    // floating just above each cell's ground top face, drawn bottom-up by layer
    // index. A cell contributes a quad on layer L only where tile(L) != 0, so
    // overlays composite roads / crops / decals over the base tile. Each overlay
    // layer has its own style: `opacity` < 1 makes the whole layer translucent
    // (engine "over" blend); `alphaCutoff` > 0 cuts decal shapes from the atlas
    // alpha. Indexed by layer; entry 0 (ground) is ignored.
    struct OverlayStyle {
        float opacity     = 1.0f;        // <1 => translucent layer
        float alphaCutoff = 0.0f;        // >0 => atlas-alpha cut-out
    };
    std::vector<OverlayStyle> overlays;
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

    // Per-cell RGB tint (0..1), multiplied into the cell's ground + overlay
    // colour. Default white (no tint). Alpha is stored but only meaningful with
    // a layer alphaCutoff. Marks the cell's chunk dirty.
    void setTint(int x, int y, float r, float g, float b, float a = 1.0f);
    void fillTint(int x0, int y0, int x1, int y1,
                  float r, float g, float b, float a = 1.0f);

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
        MeshNode* ground = nullptr;          // ground top + cliffs (nullptr until meshed)
        std::vector<MeshNode*> overlays;     // one decal mesh per overlay layer (>=1)
        bool dirty = false;
    };

    void buildChunkMesh(int ccx, int ccy);
    void buildGroundMesh(int ccx, int ccy, Chunk& chunk);
    void buildOverlayMesh(int ccx, int ccy, Chunk& chunk, int layer);
    void markCellDirty(int x, int y);
    int  chunkIdx(int ccx, int ccy) const { return ccy * chunksX_ + ccx; }
    bool solid(int x, int y) const;        // in-bounds + non-empty ground tile
    float topY(int x, int y) const;        // world Y of a cell's top surface
    void cellTint(int x, int y, float out[4]) const;  // per-cell RGBA, white default

    bool hasAtlas() const {
        return !config_.atlasPixels.empty() &&
               config_.atlasWidth > 0 && config_.atlasHeight > 0;
    }
    // Atlas cell index for a tile id (tileAtlas override or id itself).
    int atlasCellFor(uint16_t id) const {
        if (id < config_.tileAtlas.size()) return config_.tileAtlas[id];
        return static_cast<int>(id);
    }
    // Atlas cell for a cell on `layer`: an autotile rule's neighbour-selected
    // variant if (id,layer) has one, else the flat per-id cell.
    int atlasLayerCell(int x, int y, int layer, uint16_t id) const;
    // UV rect (u0,u1,v0,v1) of an atlas cell, inset by config_.atlasInset texels.
    void atlasCellRect(int cell, float& u0, float& u1, float& v0, float& v1) const;

    SceneGraph& graph_;
    TileWorldConfig config_;
    std::unique_ptr<tile::TileGrid> grid_;
    std::vector<uint32_t> tint_;   // per-cell RGBA8, row-major; 0xFFFFFFFF = none

    SceneNode* root_ = nullptr;
    int chunksX_ = 0;
    int chunksY_ = 0;
    std::vector<Chunk> chunks_;   // row-major chunksX_ * chunksY_
};

} // namespace bro::scene
