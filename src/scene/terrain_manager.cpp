#include "scene/terrain_manager.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "util/log.h"

#include <FastNoise/FastNoise.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

// -------------------------------------------------------------------------
// NoiseState — wraps FastNoise2 node tree
// -------------------------------------------------------------------------

struct TerrainManager::NoiseState {
    FastNoise::SmartNode<> node;

    void build(const TerrainConfig& cfg) {
        auto simplex = FastNoise::New<FastNoise::Simplex>();
        auto fbm = FastNoise::New<FastNoise::FractalFBm>();
        fbm->SetSource(simplex);
        fbm->SetOctaveCount(cfg.noiseOctaves);
        fbm->SetGain(cfg.noiseGain);
        fbm->SetLacunarity(cfg.noiseLacunarity);
        node = fbm;
    }
};

// -------------------------------------------------------------------------
// Construction / destruction
// -------------------------------------------------------------------------

TerrainManager::TerrainManager(SceneGraph& graph)
    : graph_(graph), noise_(std::make_unique<NoiseState>()) {}

TerrainManager::~TerrainManager() {
    clear();
}

// -------------------------------------------------------------------------
// Configuration
// -------------------------------------------------------------------------

void TerrainManager::configure(const TerrainConfig& config) {
    clear();
    config_ = config;
    noise_->build(config_);
    lastCamChunk_ = {INT_MAX, INT_MAX};
}

// -------------------------------------------------------------------------
// Coordinate helpers
// -------------------------------------------------------------------------

ChunkCoord TerrainManager::worldToChunk(float wx, float wz) const {
    float chunkWorldX = config_.chunkSizeX * config_.cellSize;
    float chunkWorldZ = config_.chunkSizeZ * config_.cellSize;
    return {
        static_cast<int>(std::floor(wx / chunkWorldX)),
        static_cast<int>(std::floor(wz / chunkWorldZ))
    };
}

void TerrainManager::worldToLocal(float wx, float wy, float wz,
                                   ChunkCoord& outChunk,
                                   int& lx, int& ly, int& lz) const {
    float chunkWorldX = config_.chunkSizeX * config_.cellSize;
    float chunkWorldZ = config_.chunkSizeZ * config_.cellSize;

    outChunk = worldToChunk(wx, wz);

    float localX = wx - outChunk.x * chunkWorldX;
    float localY = wy;
    float localZ = wz - outChunk.z * chunkWorldZ;

    lx = static_cast<int>(std::floor(localX / config_.cellSize));
    ly = static_cast<int>(std::floor(localY / config_.cellSize));
    lz = static_cast<int>(std::floor(localZ / config_.cellSize));
}

// -------------------------------------------------------------------------
// Voxel generation (noise → heightmap → voxels)
// -------------------------------------------------------------------------

void TerrainManager::generateVoxels(ChunkEntry& entry, int cx, int cz) {
    auto& chunk = *entry.voxels;
    chunk.fill(0);

    int sizeX = config_.chunkSizeX;
    int sizeY = config_.chunkSizeY;
    int sizeZ = config_.chunkSizeZ;

    // World offset of this chunk's (0,0) corner in noise space.
    // GenUniformGrid2D computes position as: index * stepSize + offset
    // For continuity across chunks, offset = chunkIndex * chunkSize * stepSize
    float worldOffX = cx * sizeX * config_.noiseFrequency;
    float worldOffZ = cz * sizeZ * config_.noiseFrequency;

    // Generate 2D heightmap using FastNoise2.
    // GenUniformGrid2D(out, xOffset, yOffset, xCount, yCount, xStep, yStep, seed)
    std::vector<float> heightmap(static_cast<size_t>(sizeX) * sizeZ);
    noise_->node->GenUniformGrid2D(heightmap.data(),
                                   worldOffX, worldOffZ,
                                   sizeX, sizeZ,
                                   config_.noiseFrequency, config_.noiseFrequency,
                                   config_.seed);

    // Material IDs (matching the existing terrain convention)
    const uint8_t BEDROCK = 4;
    const uint8_t STONE   = 3;
    const uint8_t DIRT    = 2;
    const uint8_t GRASS   = 1;
    const uint8_t SAND    = 5;

    uint8_t* voxels = chunk.data();

    for (int z = 0; z < sizeZ; z++) {
        for (int x = 0; x < sizeX; x++) {
            float raw = heightmap[z * sizeX + x];
            float t = (raw + 1.0f) * 0.5f;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;

            int h = static_cast<int>(config_.baseHeight + (t - 0.5f) * 2.0f * config_.heightAmplitude);
            if (h < 1) h = 1;
            if (h >= sizeY) h = sizeY - 1;

            for (int y = 0; y <= h && y < sizeY; y++) {
                uint8_t mat;
                if (y == 0) {
                    mat = BEDROCK;
                } else if (y == h) {
                    mat = (h <= config_.seaLevel) ? SAND : GRASS;
                } else if (y >= h - 3) {
                    mat = (h <= config_.seaLevel) ? SAND : DIRT;
                } else {
                    mat = STONE;
                }
                voxels[(z * sizeY + y) * sizeX + x] = mat;
            }
        }
    }
    chunk.markDirty();
}

// -------------------------------------------------------------------------
// Mesh building
// -------------------------------------------------------------------------

void TerrainManager::buildChunkMesh(ChunkEntry& entry, int cx, int cz) {
    auto& chunk = *entry.voxels;

    auto meshData = chunk.buildMesh(
        config_.palette.empty() ? nullptr : config_.palette.data(),
        static_cast<int>(config_.palette.size() / 4));

    if (meshData.empty()) {
        if (entry.meshNode) {
            nodeToChunk_.erase(entry.meshNode);
            graph_.destroyNode(entry.meshNode);
            entry.meshNode = nullptr;
        }
        chunk.clearDirty();
        return;
    }

    if (!entry.meshNode) {
        entry.meshNode = graph_.createMesh("terrain-chunk");
        graph_.root()->addChild(entry.meshNode);
    }

    entry.meshNode->setMesh(std::move(meshData));

    // Position the chunk in world space.
    float wx = cx * config_.chunkSizeX * config_.cellSize;
    float wz = cz * config_.chunkSizeZ * config_.cellSize;
    entry.meshNode->setPosition({wx, 0.0f, wz});

    // Update reverse map.
    nodeToChunk_[entry.meshNode] = {cx, cz};

    chunk.clearDirty();
}

// -------------------------------------------------------------------------
// Chunk loading / unloading
// -------------------------------------------------------------------------

void TerrainManager::loadChunk(int cx, int cz) {
    ChunkCoord coord{cx, cz};
    if (chunks_.count(coord)) return;

    auto& entry = chunks_[coord];
    entry.voxels = std::make_unique<bromesh::VoxelChunk>(
        config_.chunkSizeX, config_.chunkSizeY, config_.chunkSizeZ, config_.cellSize);

    generateVoxels(entry, cx, cz);
    buildChunkMesh(entry, cx, cz);
}

void TerrainManager::unloadChunk(const ChunkCoord& coord) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return;

    if (it->second.meshNode) {
        nodeToChunk_.erase(it->second.meshNode);
        graph_.destroyNode(it->second.meshNode);
    }
    chunks_.erase(it);
}

// -------------------------------------------------------------------------
// Update (camera-driven loading/unloading)
// -------------------------------------------------------------------------

int TerrainManager::update(float camX, float camY, float camZ) {
    (void)camY;

    ChunkCoord camChunk = worldToChunk(camX, camZ);
    bool camMoved = !(camChunk == lastCamChunk_);
    lastCamChunk_ = camChunk;

    int loaded = 0;

    // Load chunks within loadRadius that aren't loaded yet.
    // Build a sorted candidate list (closest first) and load up to
    // maxLoadsPerUpdate per frame to avoid stalls.
    struct LoadCandidate {
        int cx, cz, dist;
    };
    std::vector<LoadCandidate> candidates;

    int r = config_.loadRadius;
    for (int dz = -r; dz <= r; dz++) {
        for (int dx = -r; dx <= r; dx++) {
            int dist = std::abs(dx) + std::abs(dz);
            if (dist > r) continue;
            int cx = camChunk.x + dx;
            int cz = camChunk.z + dz;
            if (!chunks_.count({cx, cz})) {
                candidates.push_back({cx, cz, dist});
            }
        }
    }

    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end(),
                  [](const LoadCandidate& a, const LoadCandidate& b) {
                      return a.dist < b.dist;
                  });

        for (auto& c : candidates) {
            if (loaded >= config_.maxLoadsPerUpdate) break;
            loadChunk(c.cx, c.cz);
            loaded++;
        }
    }

    // Unload chunks beyond unloadRadius (only check when camera moved).
    if (camMoved) {
        std::vector<ChunkCoord> toUnload;
        for (auto& [coord, entry] : chunks_) {
            int dist = std::abs(coord.x - camChunk.x) + std::abs(coord.z - camChunk.z);
            if (dist > config_.unloadRadius) {
                toUnload.push_back(coord);
            }
        }
        for (auto& coord : toUnload) {
            unloadChunk(coord);
        }
    }

    return loaded;
}

// -------------------------------------------------------------------------
// Raycast
// -------------------------------------------------------------------------

TerrainHit TerrainManager::raycast(const Vec3& origin, const Vec3& dir, float maxDist) const {
    TerrainHit result;

    // Walk all loaded chunks' MeshNodes, find closest hit.
    Vec3 ndir = dir.normalized();
    if (ndir.lengthSq() < 1e-12f) return result;

    float closestDist = (maxDist > 0.0f) ? maxDist : 1e30f;
    const MeshNode* closestNode = nullptr;
    Vec3 closestWorldPos;
    Vec3 closestWorldNormal;

    for (auto& [coord, entry] : chunks_) {
        if (!entry.meshNode || !entry.meshNode->visible()) continue;

        const auto& md = entry.meshNode->mesh();
        if (md.positions.empty() || md.indices.empty()) continue;

        // Transform ray to local space.
        const Vec3& nodePos = entry.meshNode->position();
        const Quat& nodeRot = entry.meshNode->rotation();
        const Vec3& nodeScl = entry.meshNode->scale();

        Vec3 localOrigin = origin - nodePos;
        localOrigin = nodeRot.conjugate().rotate(localOrigin);
        if (nodeScl.x != 0.0f) localOrigin.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localOrigin.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localOrigin.z /= nodeScl.z;

        Vec3 localDir = nodeRot.conjugate().rotate(ndir);
        if (nodeScl.x != 0.0f) localDir.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localDir.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localDir.z /= nodeScl.z;

        float localDirLen = localDir.length();
        if (localDirLen < 1e-12f) continue;
        Vec3 localDirN = localDir * (1.0f / localDirLen);

        float scale = nodeScl.x != 0.0f ? nodeScl.x : 1.0f;
        float localMaxDist = closestDist / scale;

        // AABB early-out.
        const bromesh::BBox& lb = entry.meshNode->localBounds();
        float bmin[3] = { lb.min[0], lb.min[1], lb.min[2] };
        float bmax[3] = { lb.max[0], lb.max[1], lb.max[2] };
        float invD[3];
        for (int a = 0; a < 3; ++a) {
            float dv = (&localDirN.x)[a];
            invD[a] = (std::fabs(dv) > 1e-30f) ? 1.0f / dv
                                                : (dv >= 0.0f ? 1e30f : -1e30f);
        }
        float o[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float tmin = -1e30f, tmax = 1e30f;
        for (int a = 0; a < 3; ++a) {
            float t1 = (bmin[a] - o[a]) * invD[a];
            float t2 = (bmax[a] - o[a]) * invD[a];
            float lo = t1 < t2 ? t1 : t2;
            float hi = t1 < t2 ? t2 : t1;
            if (lo > tmin) tmin = lo;
            if (hi < tmax) tmax = hi;
        }
        if (tmax < 0.0f || tmin > tmax || tmin > localMaxDist) continue;

        // BVH raycast.
        float rayO[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float rayD[3] = { localDirN.x, localDirN.y, localDirN.z };
        bromesh::RayHit hit = entry.meshNode->bvh().raycast(md, rayO, rayD, localMaxDist);
        if (!hit.hit) continue;

        // Convert to world space.
        Vec3 localHit{hit.position[0], hit.position[1], hit.position[2]};
        localHit.x *= nodeScl.x;
        localHit.y *= nodeScl.y;
        localHit.z *= nodeScl.z;
        Vec3 worldHit = nodeRot.rotate(localHit) + nodePos;

        float worldDist = (worldHit - origin).length();
        if (worldDist >= closestDist) continue;

        closestDist = worldDist;
        closestNode = entry.meshNode;
        closestWorldPos = worldHit;

        Vec3 localNormal{hit.normal[0], hit.normal[1], hit.normal[2]};
        closestWorldNormal = nodeRot.rotate(localNormal).normalized();
    }

    if (!closestNode) return result;

    result.hit = true;
    result.worldPos[0] = closestWorldPos.x;
    result.worldPos[1] = closestWorldPos.y;
    result.worldPos[2] = closestWorldPos.z;
    result.normal[0] = closestWorldNormal.x;
    result.normal[1] = closestWorldNormal.y;
    result.normal[2] = closestWorldNormal.z;
    result.distance = closestDist;

    // Identify chunk from the reverse map.
    auto it = nodeToChunk_.find(closestNode);
    if (it != nodeToChunk_.end()) {
        result.chunk = it->second;
    }

    // Convert hit to voxel coords (nudge inward by -normal*0.5).
    float nudgeX = closestWorldPos.x - closestWorldNormal.x * 0.5f;
    float nudgeY = closestWorldPos.y - closestWorldNormal.y * 0.5f;
    float nudgeZ = closestWorldPos.z - closestWorldNormal.z * 0.5f;

    ChunkCoord hitChunk;
    int lx, ly, lz;
    worldToLocal(nudgeX, nudgeY, nudgeZ, hitChunk, lx, ly, lz);
    result.chunk = hitChunk;
    result.localX = lx;
    result.localY = ly;
    result.localZ = lz;

    // Read the material from the voxel grid.
    auto chunkIt = chunks_.find(hitChunk);
    if (chunkIt != chunks_.end()) {
        result.material = chunkIt->second.voxels->getVoxel(lx, ly, lz);
    }

    return result;
}

// -------------------------------------------------------------------------
// Voxel edits
// -------------------------------------------------------------------------

bool TerrainManager::setVoxel(float wx, float wy, float wz, uint8_t material) {
    ChunkCoord coord;
    int lx, ly, lz;
    worldToLocal(wx, wy, wz, coord, lx, ly, lz);

    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return false;

    auto& chunk = *it->second.voxels;
    if (lx < 0 || lx >= chunk.sizeX() ||
        ly < 0 || ly >= chunk.sizeY() ||
        lz < 0 || lz >= chunk.sizeZ()) return false;

    chunk.setVoxel(lx, ly, lz, material);
    return true;
}

uint8_t TerrainManager::getVoxel(float wx, float wy, float wz) const {
    ChunkCoord coord;
    int lx, ly, lz;
    worldToLocal(wx, wy, wz, coord, lx, ly, lz);

    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return 0;
    return it->second.voxels->getVoxel(lx, ly, lz);
}

void TerrainManager::rebuildDirty() {
    for (auto& [coord, entry] : chunks_) {
        if (entry.voxels->isDirty()) {
            buildChunkMesh(entry, coord.x, coord.z);
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

// -------------------------------------------------------------------------
// Cleanup
// -------------------------------------------------------------------------

void TerrainManager::clear() {
    for (auto& [coord, entry] : chunks_) {
        if (entry.meshNode) {
            graph_.destroyNode(entry.meshNode);
        }
    }
    chunks_.clear();
    nodeToChunk_.clear();
    lastCamChunk_ = {INT_MAX, INT_MAX};
}

} // namespace bro::scene
