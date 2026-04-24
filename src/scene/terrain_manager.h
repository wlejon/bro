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
    Vec3 origin = {0, 0, 0};

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
    TerrainHit raycast(const Vec3& origin, const Vec3& dir, float maxDist) const;

    /// Height sculpting at world coordinates. material=0 lowers, >0 raises.
    bool setVoxel(float wx, float wy, float wz, uint8_t material);

    /// Get material based on heightmap height at world coordinates.
    uint8_t getVoxel(float wx, float wy, float wz) const;

    /// Rebuild meshes for all dirty chunks.
    void rebuildDirty();

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
    };

    // LOD helpers
    float lodCellSize(int lod) const;
    float lodChunkWorldSize(int lod) const;
    int   lodLoadRadius(int lod) const;
    int   lodUnloadRadius(int lod) const;
    bool  isChunkCoveredByFinerLOD(int cx, int cz, int lod,
                                   float camWorldX, float camWorldZ) const;

    void generateHeightmap(ChunkEntry& entry, int cx, int cz, int lod);
    void buildChunkMesh(ChunkEntry& entry, int cx, int cz, int lod);
    void colorizeByHeight(bromesh::MeshData& mesh);
    Vec3 sphereAnchor(float flatX, float flatZ) const;
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

    // FastNoise2 state (opaque, defined in .cpp)
    struct NoiseState;
    std::unique_ptr<NoiseState> noise_;

    ChunkCoord lastCamChunk_ = {INT_MAX, INT_MAX, 0};

    // Camera altitude (updated each frame, used for altitude-aware LOD)
    float lastCamY_ = 0.0f;

};

} // namespace bro::scene
