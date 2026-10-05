#include "scene/terrain_manager.h"
#include "scene/terrain_manager_internal.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

// -------------------------------------------------------------------------
// Construction / destruction
// -------------------------------------------------------------------------

TerrainManager::TerrainManager(SceneGraph& graph)
    : graphToken_(graph.livenessToken()), noise_(std::make_unique<NoiseState>()) {}

TerrainManager::~TerrainManager() {
    clear();
}

// -------------------------------------------------------------------------
// Configuration
// -------------------------------------------------------------------------

void TerrainManager::configure(const TerrainConfig& config) {
    clear();
    config_ = config;
    // Every count here is a script's option and sizes an allocation or a
    // loop: a chunk is chunkSizeX*chunkSizeZ columns of chunkSizeY cells, the
    // streamer walks (2*loadRadius+1)^2 chunks per LOD per update, and a zero
    // chunk or cell size divides a camera position by zero. Bounded here, in
    // the one place every caller goes through.
    auto& c = config_;
    c.chunkSizeX = std::clamp(c.chunkSizeX, 1, 1024);
    c.chunkSizeY = std::clamp(c.chunkSizeY, 1, 4096);
    c.chunkSizeZ = std::clamp(c.chunkSizeZ, 1, 1024);
    if (!(c.cellSize > 0.0f) || !std::isfinite(c.cellSize)) c.cellSize = 1.0f;
    c.loadRadius = std::clamp(c.loadRadius, 0, 64);
    c.unloadRadius = std::clamp(c.unloadRadius, 0, 128);
    c.maxLoadsPerUpdate = std::clamp(c.maxLoadsPerUpdate, 0, 256);
    c.noiseOctaves = std::clamp(c.noiseOctaves, 1, 16);
    c.mountainOctaves = std::clamp(c.mountainOctaves, 0, 16);
    c.lodLevelCount = std::clamp(c.lodLevelCount, 1, 8);
    c.lodScaleFactor = std::clamp(c.lodScaleFactor, 1, 16);
    noise_->build(config_);
    lastCamChunk_ = {INT_MAX, INT_MAX, 0};
}

// -------------------------------------------------------------------------
// LOD helpers
// -------------------------------------------------------------------------

float TerrainManager::lodCellSize(int lod) const {
    float cs = config_.cellSize;
    for (int i = 0; i < lod; i++) cs *= config_.lodScaleFactor;
    return cs;
}

float TerrainManager::lodChunkWorldSize(int lod) const {
    return config_.chunkSizeX * lodCellSize(lod);
}

int TerrainManager::lodLoadRadius(int lod) const {
    if (lod == 0) {
        // At high altitudes, reduce LOD0 radius — fine detail isn't visible
        float altitude = std::max(lastCamY_, 0.0f);
        float lod0World = lodChunkWorldSize(0);
        if (altitude > lod0World * 30.0f) return 0;  // skip LOD0 entirely
        if (altitude > lod0World * 10.0f) {
            float t = (altitude - lod0World * 10.0f) / (lod0World * 20.0f);
            return std::max(2, static_cast<int>(config_.loadRadius * (1.0f - t)));
        }
        return config_.loadRadius;
    }
    // Outer LODs keep a minimum radius of 3 for a wide view
    return std::max(3, config_.loadRadius / (lod + 1));
}

int TerrainManager::lodUnloadRadius(int lod) const {
    return lodLoadRadius(lod) + 2;
}

bool TerrainManager::isChunkCoveredByFinerLOD(int cx, int cz, int lod,
                                               float camWorldX, float camWorldZ) const {
    if (lod == 0) return false;

    int finerLod = lod - 1;
    int finerRadius = lodLoadRadius(finerLod);
    float finerChunkWorld = lodChunkWorldSize(finerLod);

    // Camera chunk at finer level
    int camFinerX = static_cast<int>(std::floor(camWorldX / finerChunkWorld));
    int camFinerZ = static_cast<int>(std::floor(camWorldZ / finerChunkWorld));

    // This chunk's world bounds -> finer-level chunk coords
    int scale = config_.lodScaleFactor;
    int finerMinX = cx * scale;
    int finerMaxX = (cx + 1) * scale - 1;
    int finerMinZ = cz * scale;
    int finerMaxZ = (cz + 1) * scale - 1;

    // Check if ALL corners are within the finer level's load radius
    for (int fx : {finerMinX, finerMaxX}) {
        for (int fz : {finerMinZ, finerMaxZ}) {
            int dist = std::abs(fx - camFinerX) + std::abs(fz - camFinerZ);
            if (dist > finerRadius) return false;
        }
    }
    return true;
}

// -------------------------------------------------------------------------
// Coordinate helpers
// -------------------------------------------------------------------------

ChunkCoord TerrainManager::worldToChunk(float wx, float wz) const {
    float lx = wx - config_.origin.x;
    float lz = wz - config_.origin.z;
    float chunkWorldX = config_.chunkSizeX * config_.cellSize;
    float chunkWorldZ = config_.chunkSizeZ * config_.cellSize;
    return {
        static_cast<int>(std::floor(lx / chunkWorldX)),
        static_cast<int>(std::floor(lz / chunkWorldZ)),
        0
    };
}

void TerrainManager::worldToLocal(float wx, float wy, float wz,
                                   ChunkCoord& outChunk,
                                   int& lx, int& ly, int& lz) const {
    float chunkWorldX = config_.chunkSizeX * config_.cellSize;
    float chunkWorldZ = config_.chunkSizeZ * config_.cellSize;

    outChunk = worldToChunk(wx, wz);

    float localX = (wx - config_.origin.x) - outChunk.x * chunkWorldX;
    float localY = wy - config_.origin.y;
    float localZ = (wz - config_.origin.z) - outChunk.z * chunkWorldZ;

    lx = static_cast<int>(std::floor(localX / config_.cellSize));
    ly = static_cast<int>(std::floor(localY / config_.cellSize));
    lz = static_cast<int>(std::floor(localZ / config_.cellSize));
}

// -------------------------------------------------------------------------
// Chunk loading / unloading
// -------------------------------------------------------------------------

void TerrainManager::loadChunk(int cx, int cz, int lod) {
    ChunkCoord coord{cx, cz, lod};
    if (chunks_.count(coord)) return;

    auto& entry = chunks_[coord];
    generateHeightmap(entry, cx, cz, lod);
    buildChunkMesh(entry, cx, cz, lod);
}

void TerrainManager::unloadChunk(const ChunkCoord& coord) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return;

    if (it->second.meshNode) {
        nodeToChunk_.erase(it->second.meshNode);
        if (auto* g = graph()) g->destroyNode(it->second.meshNode);
    }
    chunks_.erase(it);
}

// -------------------------------------------------------------------------
// Update (camera-driven loading/unloading, multi-LOD)
// -------------------------------------------------------------------------

int TerrainManager::update(float camX, float camY, float camZ) {
    lastCamY_ = camY;

    // Convert world-space camera to terrain-local coordinates
    float localCamX = camX - config_.origin.x;
    float localCamZ = camZ - config_.origin.z;

    ChunkCoord camChunk = worldToChunk(camX, camZ);
    bool camMoved = !(camChunk == lastCamChunk_);
    lastCamChunk_ = camChunk;

    int totalLoaded = 0;
    int maxPerFrame = config_.maxLoadsPerUpdate;
    int levelCount = std::max(1, config_.lodLevelCount);

    // Refresh stale chunks before loading new ones. They share the budget: a
    // chunk that exists but shows placeholder data is worth more than one that
    // does not exist yet, and letting both run at full budget in the same frame
    // is how a streaming source turns into a stutter.
    totalLoaded += processRegen(maxPerFrame);
    maxPerFrame -= totalLoaded;
    if (maxPerFrame <= 0) return totalLoaded;

    struct LoadCandidate {
        int cx, cz, lod, dist;
    };

    for (int lod = 0; lod < levelCount; lod++) {
        float chunkWorld = lodChunkWorldSize(lod);

        // Camera chunk at this LOD level (in terrain-local space)
        int camCX = static_cast<int>(std::floor(localCamX / chunkWorld));
        int camCZ = static_cast<int>(std::floor(localCamZ / chunkWorld));

        int r = lodLoadRadius(lod);

        // Build load candidates
        std::vector<LoadCandidate> candidates;
        for (int dz = -r; dz <= r; dz++) {
            for (int dx = -r; dx <= r; dx++) {
                int dist = std::abs(dx) + std::abs(dz);
                if (dist > r) continue;
                int cx = camCX + dx;
                int cz = camCZ + dz;

                // Skip if covered by finer LOD
                if (lod > 0 && isChunkCoveredByFinerLOD(cx, cz, lod, localCamX, localCamZ))
                    continue;

                ChunkCoord coord{cx, cz, lod};
                if (!chunks_.count(coord)) {
                    candidates.push_back({cx, cz, lod, dist});
                }
            }
        }

        if (!candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(),
                      [](const LoadCandidate& a, const LoadCandidate& b) {
                          return a.dist < b.dist;
                      });

            for (auto& c : candidates) {
                if (totalLoaded >= maxPerFrame) break;
                loadChunk(c.cx, c.cz, c.lod);
                totalLoaded++;
            }
        }

        // Unload chunks beyond radius OR now covered by finer LOD
        if (camMoved) {
            std::vector<ChunkCoord> toUnload;
            for (auto& [coord, entry] : chunks_) {
                if (coord.lod != lod) continue;
                int dist = std::abs(coord.x - camCX) + std::abs(coord.z - camCZ);
                if (dist > lodUnloadRadius(lod) ||
                    (lod > 0 && isChunkCoveredByFinerLOD(coord.x, coord.z, lod, localCamX, localCamZ))) {
                    toUnload.push_back(coord);
                }
            }
            for (auto& coord : toUnload) {
                unloadChunk(coord);
            }
        }
    }

    // Dynamically update nearClipDist: only clip a LOD's fragments when finer
    // LOD chunks are actually loaded to replace them.  Without this, distant
    // planets whose finer LODs aren't loaded would clip to nothing.
    std::vector<bool> lodHasChunks(levelCount, false);
    for (auto& [coord, entry] : chunks_) {
        if (coord.lod < levelCount) lodHasChunks[coord.lod] = true;
    }
    for (auto& [coord, entry] : chunks_) {
        if (!entry.meshNode || coord.lod == 0) continue;
        int finerLod = coord.lod - 1;
        if (finerLod < levelCount && lodHasChunks[finerLod]) {
            float finerCoverage = lodChunkWorldSize(finerLod) * lodLoadRadius(finerLod);
            // lodLoadRadius is a MANHATTAN radius, so the finer level covers a
            // diamond, not a disc. A diamond of radius R only reaches R/sqrt(2)
            // along its diagonals, so clipping this ring at 0.9*R cut away
            // coarse fragments in the diagonal directions where no finer chunk
            // had been loaded to replace them — punching wedge-shaped holes
            // clean through the terrain, showing sky or water beneath.
            // 0.65 stays inside the diamond's inscribed circle (0.707) with
            // margin for the chunks being squares rather than points.
            entry.meshNode->setNearClipDist(finerCoverage * 0.65f);
        } else {
            entry.meshNode->setNearClipDist(0.0f);
        }
    }

    return totalLoaded;
}

// -------------------------------------------------------------------------
// Voxel edits (LOD 0 only)
// -------------------------------------------------------------------------

bool TerrainManager::setVoxel(float wx, float wy, float wz, uint8_t material) {
    ChunkCoord coord;
    int lx, ly, lz;
    worldToLocal(wx, wy, wz, coord, lx, ly, lz);

    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return false;

    int gridW = config_.chunkSizeX + 1;
    int gridH = config_.chunkSizeZ + 1;
    if (lx < 0 || lx >= gridW || lz < 0 || lz >= gridH) return false;

    ChunkEntry& entry = it->second;
    float delta = (material == 0) ? -1.0f : 1.0f;
    entry.heightmap[lz * gridW + lx] += delta;

    // buildChunkMesh() builds exclusively from heightmapPadded (every mesh
    // mode reads it, never heightmap), so the edit must land there too,
    // offset by the 1-cell border shared with neighbouring chunks.
    int paddedW = gridW + 2;
    entry.heightmapPadded[(lz + 1) * paddedW + (lx + 1)] += delta;

    entry.dirty_ = true;
    return true;
}

uint8_t TerrainManager::getVoxel(float wx, float wy, float wz) const {
    ChunkCoord coord;
    int lx, ly, lz;
    worldToLocal(wx, wy, wz, coord, lx, ly, lz);

    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return 0;

    int gridW = config_.chunkSizeX + 1;
    if (lx < 0 || lx >= gridW || lz < 0 || lz >= config_.chunkSizeZ + 1) return 0;

    float h = it->second.heightmap[lz * gridW + lx];
    return (wy <= h) ? 1 : 0;
}

void TerrainManager::invalidateRegion(float wx0, float wz0, float wx1, float wz1) {
    if (wx1 < wx0) std::swap(wx0, wx1);
    if (wz1 < wz0) std::swap(wz0, wz1);

    for (auto& [coord, entry] : chunks_) {
        const float cell   = lodCellSize(coord.lod);
        const float chunkW = config_.chunkSizeX * cell;
        const float chunkD = config_.chunkSizeZ * cell;
        const float x0 = config_.origin.x + coord.x * chunkW;
        const float z0 = config_.origin.z + coord.z * chunkD;

        // The padded ring reaches one cell past the chunk on every side, so a
        // chunk whose interior misses the region may still have sampled inside
        // it — and a stale skirt is a seam against the neighbour that updated.
        if (x0 + chunkW + cell < wx0 || x0 - cell > wx1) continue;
        if (z0 + chunkD + cell < wz0 || z0 - cell > wz1) continue;
        entry.needsRegen_ = true;
    }
}

// Regenerate a bounded number of stale chunks. Called from update() so the work
// is spread across frames the same way chunk loading is.
int TerrainManager::processRegen(int budget) {
    int done = 0;
    std::vector<float> previous;
    for (auto& [coord, entry] : chunks_) {
        if (done >= budget) break;
        if (!entry.needsRegen_) continue;
        entry.needsRegen_ = false;

        previous = entry.heightmapPadded;
        generateHeightmap(entry, coord.x, coord.z, coord.lod);

        // Most invalidated chunks are unchanged — an arriving tile is far
        // larger than the region whose answers were actually placeholders — and
        // remeshing them would burn the whole budget on no visible difference.
        const bool changed =
            previous.size() != entry.heightmapPadded.size() ||
            std::memcmp(previous.data(), entry.heightmapPadded.data(),
                        previous.size() * sizeof(float)) != 0;
        if (changed) buildChunkMesh(entry, coord.x, coord.z, coord.lod);

        done++;
    }
    return done;
}

void TerrainManager::rebuildDirty() {
    for (auto& [coord, entry] : chunks_) {
        if (entry.dirty_) {
            buildChunkMesh(entry, coord.x, coord.z, coord.lod);
            entry.dirty_ = false;
        }
    }
}

// -------------------------------------------------------------------------
// Stats
// -------------------------------------------------------------------------

int TerrainManager::totalTriangles() const {
    int total = 0;
    for (auto& [coord, entry] : chunks_) {
        if (entry.meshNode) {
            total += static_cast<int>(entry.meshNode->mesh().triangleCount());
        }
    }
    return total;
}

int TerrainManager::totalVertices() const {
    int total = 0;
    for (auto& [coord, entry] : chunks_) {
        if (entry.meshNode) {
            total += static_cast<int>(entry.meshNode->mesh().vertexCount());
        }
    }
    return total;
}

float TerrainManager::farDistance() const {
    int maxLod = std::max(1, config_.lodLevelCount) - 1;
    return lodChunkWorldSize(maxLod) * lodLoadRadius(maxLod);
}

// -------------------------------------------------------------------------
// Cleanup
// -------------------------------------------------------------------------

void TerrainManager::clear() {
    // A reclaimed graph already destroyed these nodes with itself; the
    // pointers are freed memory by the time ~TerrainManager runs.
    if (auto* g = graph()) {
        for (auto& [coord, entry] : chunks_) {
            if (entry.meshNode) g->destroyNode(entry.meshNode);
        }
    }
    chunks_.clear();
    nodeToChunk_.clear();
    lastCamChunk_ = {INT_MAX, INT_MAX, 0};
}

} // namespace bro::scene
