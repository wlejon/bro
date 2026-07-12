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
#include <bromesh/mesh_data.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::scene {

class SceneGraph;
class SceneNode;
class MeshNode;
class InstancedMeshNode;

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

    // ---- animated tiles (optional; require an atlas) --------------------
    // A tile id can cycle through a sequence of atlas cells over time (flowing
    // water, swaying crops, torch flicker). The app drives it via
    // TileWorld::advance(dtMs); only chunks containing animated tiles remesh,
    // and only when the frame actually changes. Autotile rules take precedence
    // over animation for the same id+layer.
    struct TileAnimation {
        uint16_t id = 0;                 // tile id this animation applies to
        float    fps = 4.0f;             // frames per second
        std::vector<int> frames;         // atlas cell per frame
    };
    std::vector<TileAnimation> animations;
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

    // Replace the grid with a pre-populated one (e.g. from tile::deserialize),
    // sync config_'s dimensions/topology/layers to match, and remesh every
    // chunk. Rendering config (cellSize, atlas, autotiles, overlays,
    // animations, ...) is preserved from the current configuration.
    void loadGrid(tile::TileGrid&& newGrid);

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
    // Stored tint of a cell, packed RGBA8 (as quantized by setTint — 8 bits per
    // channel, inputs clamped to 0..1). 0xFFFFFFFF (white) for OOB / untinted.
    uint32_t tintAt(int x, int y) const;

    // ---- query ----------------------------------------------------------
    uint16_t tile(int x, int y, int layer = 0) const;
    int      elevation(int x, int y) const;
    bool     hasFlag(int x, int y, uint32_t bit) const;

    // Map a world XZ position to a grid cell (ignores elevation — a flat
    // top-down projection). Returns false if outside the grid.
    bool worldToCell(float wx, float wz, int& outX, int& outY) const;

    // World-space XZ center of a cell (topology-aware: square cell center or hex
    // pointy-top pixel center). Used for object anchoring and nav-grid stamping.
    void cellCenterWorldXZ(int x, int y, float& outX, float& outZ) const;

    // Axis-aligned XZ bounding box of the whole grid in world space (topology-aware —
    // hex's bounds aren't a clean width*cellSize box). Used to size nav-grid export.
    void worldBounds(float& minX, float& minZ, float& maxX, float& maxZ) const;

    // ---- picking / collision -------------------------------------------
    // Result of a ray->cell pick. `side` is true when the ray struck a cliff
    // face rather than a cell's flat top surface.
    struct CellRayHit {
        bool  hit = false;
        int   x = 0, y = 0;
        float point[3] = {0, 0, 0};   // world-space hit position
        float distance = 0.0f;
        bool  side = false;
    };
    // Cast a world-space ray against the tile surface — the per-cell top faces
    // and the cliff sides between them — front-to-back with elevation
    // occlusion (a near plateau hides the ground behind it). Returns the first
    // solid cell hit (analytic grid DDA; no BVH, no mesh dependency).
    CellRayHit raycastCell(const bromath::Vec3& origin, const bromath::Vec3& dir,
                           float maxDist = 1.0e6f) const;

    // World Y of the top surface of the cell under a world XZ position — the
    // ground height an agent stands at. Returns false off-grid or over an
    // empty (hole) cell.
    bool sampleHeight(float wx, float wz, float& outY) const;

    // A cell is walkable when it carries ground (non-empty tile on layer 0) and
    // none of `blockMask`'s flag bits are set on it. blockMask 0 => every solid
    // cell is walkable. Feeds nav-grid export.
    bool isWalkable(int x, int y, uint32_t blockMask = 0) const;

    // ---- placement ------------------------------------------------------
    void setOrigin(float x, float y, float z);
    SceneNode* rootNode() const { return root_; }

    // ---- animation ------------------------------------------------------
    // Advance animated tiles by `dtMs` milliseconds. Remeshes only the chunks
    // that contain animated tiles, and only when a frame index changed. Returns
    // true if anything was remeshed (so the caller can skip a redundant flush).
    bool advance(double dtMs);

    // ---- objects / entities --------------------------------------------
    // Real 3D props placed on cells, GPU-instanced: one shared mesh per "kind"
    // drawn across all its placements in a single draw call. Each kind owns an
    // InstancedMeshNode under the world root; placements anchor to a cell's
    // top-surface centre. Author with addObject(), then rebuild().
    struct ObjectStyle {
        float color[4]   = {1, 1, 1, 1};
        float roughness  = 0.8f;
        float metallic   = 0.0f;
        bool  doubleSided = false;   // for leaf-card / billboard props
        float alphaCutoff = 0.0f;    // >0 cuts cut-out textures
        bool  castsShadow = true;
        int   atlasCols = 1;         // texture atlas grid (variant selects a cell)
        int   atlasRows = 1;
        // Optional baseColor texture (RGBA8). Empty -> untextured.
        std::vector<uint8_t> texPixels;
        int   texWidth = 0;
        int   texHeight = 0;
    };
    struct ObjectPlacement {
        float yaw     = 0.0f;        // radians about Y
        float scale   = 1.0f;        // uniform
        float yOffset = 0.0f;        // lift above the cell top
        float offsetX = 0.0f;        // sub-cell offset in cell units (-0.5..0.5)
        float offsetZ = 0.0f;
        int   variant = 0;           // atlas cell when the kind has an atlas
        float color[4] = {1, 1, 1, 1};  // per-instance tint (a = opacity, or
                                         // variant index when atlasCols/Rows>1)
    };

    // Register a prop kind from a mesh + material; returns its kind index (-1 on
    // failure). The mesh is moved in.
    int  addObjectKind(bromesh::MeshData&& mesh, const ObjectStyle& style);
    int  objectKindCount() const { return static_cast<int>(objectKinds_.size()); }

    // Place an instance of `kind` on cell (x, y). Returns the instance index
    // within that kind, or -1 on a bad kind/cell. Marks the kind for rebuild.
    int  addObject(int kind, int x, int y, const ObjectPlacement& p);
    // Remove all placements of `kind` (or every kind when kind < 0).
    void clearObjects(int kind = -1);
    int  objectCount(int kind) const;

    // Flush object-kind instance buffers changed since the last rebuild.
    void rebuildObjects();

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

    // Shared tail of configure()/loadGrid(): (re)build chunk/tint/animation
    // storage and the root node from the current config_ + grid_, then
    // remesh everything. Assumes grid_ is already installed.
    void initFromGrid();

    void buildChunkMesh(int ccx, int ccy);
    void buildGroundMesh(int ccx, int ccy, Chunk& chunk);
    void buildOverlayMesh(int ccx, int ccy, Chunk& chunk, int layer);
    void markCellDirty(int x, int y);
    int  chunkIdx(int ccx, int ccy) const { return ccy * chunksX_ + ccx; }
    bool solid(int x, int y) const;        // in-bounds + non-empty ground tile
    float topY(int x, int y) const;        // world Y of a cell's top surface
    void cellTint(int x, int y, float out[4]) const;  // per-cell RGBA, white default

    // Grid-local (no origin) XZ center of a cell; square cell-center or hex
    // pointy-top pixel center depending on grid_->topology().
    void cellCenterLocal(int x, int y, float& px, float& pz) const;
    // Inverse of the hex branch of cellCenterLocal: nearest hex cell to a
    // grid-local XZ point (may be out of grid bounds — caller checks).
    tile::Cell pixelToHexCell(float lx, float lz) const;

    bool hasAtlas() const {
        return !config_.atlasPixels.empty() &&
               config_.atlasWidth > 0 && config_.atlasHeight > 0;
    }
    // Atlas cell index for a tile id: the current animation frame if the id is
    // animated, else the tileAtlas override, else the id itself.
    int atlasCellFor(uint16_t id) const {
        if (id < animOf_.size() && animOf_[id] >= 0) {
            const auto& a = config_.animations[animOf_[id]];
            if (!a.frames.empty()) {
                int f = animFrame_[animOf_[id]] % static_cast<int>(a.frames.size());
                return a.frames[f];
            }
        }
        if (id < config_.tileAtlas.size()) return config_.tileAtlas[id];
        return static_cast<int>(id);
    }
    bool cellIsAnimated(uint16_t id) const {
        return id < animOf_.size() && animOf_[id] >= 0;
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

    // Animation state
    std::vector<int>  animOf_;        // tile id -> animation index, or -1
    std::vector<int>  animFrame_;     // current frame per animation
    std::vector<char> chunkAnimated_; // per-chunk: contains an animated tile
    double animClock_ = 0.0;          // accumulated ms

    // Object kinds (GPU-instanced props). One InstancedMeshNode per kind.
    struct ObjectKind {
        InstancedMeshNode* node = nullptr;
        std::vector<ObjectPlacement> placements;  // parallel to cell coords
        std::vector<int> cellX, cellY;            // cell per placement
        bool dirty = false;
    };
    std::vector<ObjectKind> objectKinds_;
    void rebuildObjectKind(ObjectKind& k);

    SceneNode* root_ = nullptr;
    int chunksX_ = 0;
    int chunksY_ = 0;
    std::vector<Chunk> chunks_;   // row-major chunksX_ * chunksY_
};

} // namespace bro::scene
