#pragma once

#include "scene/scene_node.h"
#include <bromesh/mesh_data.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::scene {

class SceneGraph;
class MeshNode;

// -------------------------------------------------------------------------
// Configuration
// -------------------------------------------------------------------------

struct TerrainConfig {
    int chunkSizeX = 64;
    int chunkSizeY = 48;
    int chunkSizeZ = 64;
    float cellSize = 1.0f;

    int loadRadius   = 4;   // Manhattan distance in chunks
    int unloadRadius = 6;
    int maxLoadsPerUpdate = 2;

    // Noise parameters (built-in FBm generator)
    int   seed           = 1337;
    float noiseFrequency = 0.035f;
    int   noiseOctaves   = 5;
    float noiseGain      = 0.5f;
    float noiseLacunarity = 2.0f;

    // Terrain shape
    int baseHeight      = 18;
    int heightAmplitude = 16;
    int seaLevel        = 14;

    // Mesh mode: 0=smooth, 1=flat, 2=terraced, 3=blocky
    int meshMode = 0;

    // Terraced mode: step height in world units
    float terraceStep = 1.0f;

    // Continental noise — large-scale amplitude modulation for mountain ranges.
    // 0 = disabled (uniform amplitude). >0 adds regional variation.
    float continentFrequency = 0.0f;   // very low (e.g. 0.002)
    float continentMin       = 0.1f;   // amplitude scale in plains
    float continentMax       = 1.5f;   // amplitude scale in mountain ranges

    // LOD ring configuration
    int   lodLevelCount  = 1;       // 1 = no LOD (current behavior)
    int   lodScaleFactor = 4;       // each level is N× coarser
    float planetRadius   = 0.0f;    // 0 = flat; 6371000 = Earth curvature

    // Mountain pass — enormous low-frequency features layered on top
    float mountainFrequency = 0.0f;   // 0 = disabled
    float mountainAmplitude = 0.0f;
    int   mountainOctaves   = 3;

    // World-space origin of this terrain. Allows multiple terrains at
    // different positions (e.g. multiple planets).
    bromath::Vec3 origin = {0, 0, 0};

    // Material palette: RGBA floats, 4 per material ID. Index 0 = air.
    std::vector<float> palette;
};

// -------------------------------------------------------------------------
// Chunk coordinate (includes LOD level)
// -------------------------------------------------------------------------

struct ChunkCoord {
    int x   = 0;
    int z   = 0;
    int lod = 0;
    bool operator==(const ChunkCoord& o) const {
        return x == o.x && z == o.z && lod == o.lod;
    }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const {
        auto h1 = std::hash<int>()(c.x);
        auto h2 = std::hash<int>()(c.z);
        auto h3 = std::hash<int>()(c.lod);
        return h1 ^ (h2 * 2654435761u) ^ (h3 * 2246822519u);
    }
};

// -------------------------------------------------------------------------
// Terrain hit result
// -------------------------------------------------------------------------

struct TerrainHit {
    bool hit = false;
    float worldPos[3] = {};
    float normal[3]   = {};
    float distance     = 0.0f;
    ChunkCoord chunk;
    int localX = 0, localY = 0, localZ = 0;
    uint8_t material = 0;
};

// -------------------------------------------------------------------------
// TerrainManager
// -------------------------------------------------------------------------

/// Manages an infinite heightmap terrain with multi-LOD rings and optional
/// planetary curvature. Creates MeshNodes in a SceneGraph. Drives chunk
/// loading/unloading from camera position.
class TerrainManager {
public:
    explicit TerrainManager(SceneGraph& graph);
    ~TerrainManager();

    TerrainManager(const TerrainManager&) = delete;
    TerrainManager& operator=(const TerrainManager&) = delete;

    /// Apply configuration. Clears and rebuilds all chunks.
    void configure(const TerrainConfig& config);
    const TerrainConfig& config() const { return config_; }

    /// Update chunk loading/unloading based on camera position.
    /// Returns the number of chunks loaded this frame.
    int update(float camX, float camY, float camZ);

    /// Terrain-specific raycast (LOD 0 only). Returns chunk + voxel info.
    TerrainHit raycast(const bromath::Vec3& origin, const bromath::Vec3& dir, float maxDist) const;

    /// Height sculpting at world coordinates. material=0 lowers, >0 raises.
    bool setVoxel(float wx, float wy, float wz, uint8_t material);

    /// Get material based on heightmap height at world coordinates.
    uint8_t getVoxel(float wx, float wy, float wz) const;

    /// Rebuild meshes for all dirty chunks.
    void rebuildDirty();

    /// Supplies a chunk's heights in place of the built-in FBm generator.
    ///
    /// Called with the PADDED grid — paddedW * paddedH floats, row-major and
    /// z-major, so sample (px, pz) is at `padded[pz * paddedW + px]`. Values are
    /// absolute world-space Y in world units, exactly as the noise path writes
    /// them. The padded ring is one sample beyond the chunk on every side and is
    /// shared with the neighbours, which is what lets normals use true central
    /// differences at a chunk edge; a provider that is not consistent across
    /// chunk boundaries will show seams there.
    ///
    /// `worldX0`/`worldZ0` are the world position of sample (0, 0), so the
    /// provider never has to re-derive the skirt offset:
    ///     worldX = worldX0 + px * cellSize
    ///     worldZ = worldZ0 + pz * cellSize
    /// Getting that offset wrong shifts a chunk by one cell against its
    /// neighbours, which for a coherent (non-noise) source yields terrain that
    /// looks entirely correct and simply does not line up.
    ///
    /// Note these positions include config.origin, while the built-in noise path
    /// does not — a pre-existing quirk that only matters for a non-zero origin.
    ///
    /// Return false to fall back to the built-in noise for this chunk, which is
    /// what makes layering a coarse learned source under FBm detail possible.
    using HeightSource =
        std::function<bool(int cx, int cz, int lod, float* padded,
                           int paddedW, int paddedH, float cellSize,
                           float worldX0, float worldZ0)>;

    /// Install (or clear, with nullptr) the height source. Does not rebuild —
    /// call configure() or clear() to regenerate existing chunks.
    void setHeightSource(HeightSource fn) { heightSource_ = std::move(fn); }

    /// Re-ask the height source for every chunk overlapping a world-space XZ
    /// region, because the data behind it changed.
    ///
    /// This exists for streaming height sources. Such a source has to serve
    /// chunks before its data arrives — with a placeholder, or by declining —
    /// and needs a way to say "that answer is stale now" when it does arrive.
    /// The only tool for that used to be configure(), which clear()s every
    /// chunk and rebuilds from nothing; with data landing every few seconds
    /// the terrain is wiped faster than the load budget can refill it and it
    /// visibly appears and disappears on repeat, forever.
    ///
    /// Nothing is destroyed here. Affected chunks keep their MeshNode and are
    /// regenerated in place, a few per update() against maxLoadsPerUpdate, and
    /// a chunk whose heights come back byte-identical does not even rebuild
    /// its mesh. So terrain refines rather than blinking.
    ///
    /// The region is expanded by one cell internally: a chunk's padded ring
    /// samples into its neighbours, so a chunk just outside the region can
    /// still have read data from inside it.
    void invalidateRegion(float wx0, float wz0, float wx1, float wz1);

    /// Stats.
    int chunkCount() const { return static_cast<int>(chunks_.size()); }
    int totalTriangles() const;
    int totalVertices() const;

    /// World distance to edge of outermost LOD ring.
    float farDistance() const;

    /// Destroy all chunks and reset state.
    void clear();

private:
    struct ChunkEntry {
        // Interior heightmap, gridW * gridH floats. Used for gameplay queries
        // (raycasts, material at world-xz, etc.) where the boundary-matching
        // skirt is irrelevant.
        std::vector<float> heightmap;
        // Padded heightmap, (gridW + 2) * (gridH + 2) floats, sharing values
        // with the neighbouring chunks' boundary rows. Used for seam-free
        // mesh building so normals at shared edges use true central
        // differences and voxel-neighbour queries see across the chunk edge.
        std::vector<float> heightmapPadded;
        MeshNode* meshNode = nullptr;   // owned by SceneGraph
        bool dirty_ = false;            // needs mesh rebuild
        bool needsRegen_ = false;       // heights are stale; re-ask the source
    };

    // LOD helpers
    float lodCellSize(int lod) const;
    float lodChunkWorldSize(int lod) const;
    int   lodLoadRadius(int lod) const;
    int   lodUnloadRadius(int lod) const;
    bool  isChunkCoveredByFinerLOD(int cx, int cz, int lod,
                                   float camWorldX, float camWorldZ) const;

    int  processRegen(int budget);
    void generateHeightmap(ChunkEntry& entry, int cx, int cz, int lod);
    void buildChunkMesh(ChunkEntry& entry, int cx, int cz, int lod);
    void colorizeByHeight(bromesh::MeshData& mesh);
    bromath::Vec3 sphereAnchor(float flatX, float flatZ) const;
    void applyCurvatureToMesh(bromesh::MeshData& mesh, float chunkCenterX,
                              float chunkCenterZ) const;
    void loadChunk(int cx, int cz, int lod);
    void unloadChunk(const ChunkCoord& coord);

    ChunkCoord worldToChunk(float wx, float wz) const;
    void worldToLocal(float wx, float wy, float wz,
                      ChunkCoord& outChunk, int& lx, int& ly, int& lz) const;

    SceneGraph& graph_;
    TerrainConfig config_;

    std::unordered_map<ChunkCoord, ChunkEntry, ChunkCoordHash> chunks_;
    std::unordered_map<const MeshNode*, ChunkCoord> nodeToChunk_;

    HeightSource heightSource_;

    // FastNoise2 state (opaque, defined in .cpp)
    struct NoiseState;
    std::unique_ptr<NoiseState> noise_;

    ChunkCoord lastCamChunk_ = {INT_MAX, INT_MAX, 0};

    // Camera altitude (updated each frame, used for altitude-aware LOD)
    float lastCamY_ = 0.0f;

};

} // namespace bro::scene
