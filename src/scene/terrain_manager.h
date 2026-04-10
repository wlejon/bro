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

    // Material palette: RGBA floats, 4 per material ID. Index 0 = air.
    std::vector<float> palette;
};

// -------------------------------------------------------------------------
// Chunk coordinate
// -------------------------------------------------------------------------

struct ChunkCoord {
    int x = 0;
    int z = 0;
    bool operator==(const ChunkCoord& o) const { return x == o.x && z == o.z; }
};

struct ChunkCoordHash {
    size_t operator()(const ChunkCoord& c) const {
        // Spread bits to reduce collisions on grid-aligned coords.
        auto h1 = std::hash<int>()(c.x);
        auto h2 = std::hash<int>()(c.z);
        return h1 ^ (h2 * 2654435761u);
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

/// Manages an infinite voxel terrain. Owns VoxelChunks and creates MeshNodes
/// in a SceneGraph. Drives chunk loading/unloading from camera position.
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

    /// Terrain-specific raycast. Returns chunk + voxel info.
    TerrainHit raycast(const Vec3& origin, const Vec3& dir, float maxDist) const;

    /// Set a voxel at world coordinates. Finds the chunk, sets voxel, marks dirty.
    /// material=0 removes the block. Returns false if chunk not loaded or OOB.
    bool setVoxel(float wx, float wy, float wz, uint8_t material);

    /// Get a voxel at world coordinates. Returns 0 if chunk not loaded or OOB.
    uint8_t getVoxel(float wx, float wy, float wz) const;

    /// Rebuild meshes for all dirty chunks.
    void rebuildDirty();

    /// Stats.
    int chunkCount() const { return static_cast<int>(chunks_.size()); }
    int totalTriangles() const;
    int totalVertices() const;

    /// Destroy all chunks and reset state.
    void clear();

private:
    struct ChunkEntry {
        std::vector<float> heightmap;   // gridW * gridH float heights
        MeshNode* meshNode = nullptr;   // owned by SceneGraph
        bool dirty_ = false;            // needs mesh rebuild
    };

    void generateHeightmap(ChunkEntry& entry, int cx, int cz);
    void buildChunkMesh(ChunkEntry& entry, int cx, int cz);
    void colorizeByHeight(bromesh::MeshData& mesh);
    void loadChunk(int cx, int cz);
    void unloadChunk(const ChunkCoord& coord);

    ChunkCoord worldToChunk(float wx, float wz) const;
    void worldToLocal(float wx, float wy, float wz,
                      ChunkCoord& outChunk, int& lx, int& ly, int& lz) const;

    SceneGraph& graph_;
    TerrainConfig config_;

    std::unordered_map<ChunkCoord, ChunkEntry, ChunkCoordHash> chunks_;

    // Reverse map: MeshNode pointer → ChunkCoord (for raycast identification)
    std::unordered_map<const MeshNode*, ChunkCoord> nodeToChunk_;

    // FastNoise2 state (opaque, defined in .cpp)
    struct NoiseState;
    std::unique_ptr<NoiseState> noise_;

    ChunkCoord lastCamChunk_ = {INT_MAX, INT_MAX};
};

} // namespace bro::scene
