#include "scene/terrain_manager.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"
#include "util/log.h"

#include <FastNoise/FastNoise.h>
#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/voxel/voxel_chunk.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

// -------------------------------------------------------------------------
// NoiseState — wraps FastNoise2 node tree
// -------------------------------------------------------------------------

struct TerrainManager::NoiseState {
    FastNoise::SmartNode<> node;
    float maxAmplitude = 1.0f;  // theoretical peak of FBm output

    void build(const TerrainConfig& cfg) {
        auto simplex = FastNoise::New<FastNoise::Simplex>();
        auto fbm = FastNoise::New<FastNoise::FractalFBm>();
        fbm->SetSource(simplex);
        fbm->SetOctaveCount(cfg.noiseOctaves);
        fbm->SetGain(cfg.noiseGain);
        fbm->SetLacunarity(cfg.noiseLacunarity);
        node = fbm;

        // Compute the theoretical max amplitude of the FBm sum:
        //   sum(gain^i, i=0..octaves-1)
        // This lets us normalize the output to [-1, 1] regardless of settings.
        float amp = 0.0f;
        float g = 1.0f;
        for (int i = 0; i < cfg.noiseOctaves; i++) {
            amp += g;
            g *= cfg.noiseGain;
        }
        maxAmplitude = std::max(amp, 1.0f);
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
// Heightmap generation (noise → height values)
// -------------------------------------------------------------------------

void TerrainManager::generateHeightmap(ChunkEntry& entry, int cx, int cz) {
    // Grid has +1 vertices in each direction to produce sizeX * sizeZ quads.
    int gridW = config_.chunkSizeX + 1;
    int gridH = config_.chunkSizeZ + 1;
    entry.heightmap.resize(static_cast<size_t>(gridW) * gridH);

    // World offset of this chunk's (0,0) corner in noise space.
    // GenUniformGrid2D: position = index * step + offset
    // For continuity across chunks: offset = chunkIndex * chunkSize * step
    float worldOffX = cx * config_.chunkSizeX * config_.noiseFrequency;
    float worldOffZ = cz * config_.chunkSizeZ * config_.noiseFrequency;

    noise_->node->GenUniformGrid2D(entry.heightmap.data(),
                                   worldOffX, worldOffZ,
                                   gridW, gridH,
                                   config_.noiseFrequency, config_.noiseFrequency,
                                   config_.seed);

    // Normalize FBm output by theoretical amplitude and convert to world heights.
    float invAmp = 1.0f / noise_->maxAmplitude;
    for (size_t i = 0; i < entry.heightmap.size(); i++) {
        float raw = entry.heightmap[i];
        float t = (raw * invAmp + 1.0f) * 0.5f;
        t = std::clamp(t, 0.0f, 1.0f);
        entry.heightmap[i] = config_.baseHeight + (t - 0.5f) * 2.0f * config_.heightAmplitude;
    }
}

// -------------------------------------------------------------------------
// Vertex coloring helpers
// -------------------------------------------------------------------------

void TerrainManager::colorizeByHeight(bromesh::MeshData& mesh) {
    size_t vertCount = mesh.vertexCount();
    mesh.colors.resize(vertCount * 4);

    auto samplePalette = [&](int matID, float& r, float& g, float& b) {
        int matCount = static_cast<int>(config_.palette.size() / 4);
        if (matID < 0 || matID >= matCount) { r = g = b = 0.5f; return; }
        int off = matID * 4;
        r = config_.palette[off];
        g = config_.palette[off + 1];
        b = config_.palette[off + 2];
    };

    float seaF = static_cast<float>(config_.seaLevel);
    float grassTop = seaF + config_.heightAmplitude * 0.6f;
    float stoneBot = seaF + config_.heightAmplitude * 0.85f;

    for (size_t i = 0; i < vertCount; i++) {
        float y = mesh.positions[i * 3 + 1];
        float r, g, b;

        if (y <= seaF) {
            samplePalette(5, r, g, b);  // sand
        } else if (y <= grassTop) {
            float t = (y - seaF) / std::max(grassTop - seaF, 0.01f);
            float gr, gg, gb, dr, dg, db;
            samplePalette(1, gr, gg, gb);
            samplePalette(2, dr, dg, db);
            r = gr + (dr - gr) * t * t;
            g = gg + (dg - gg) * t * t;
            b = gb + (db - gb) * t * t;
        } else if (y <= stoneBot) {
            float t = (y - grassTop) / std::max(stoneBot - grassTop, 0.01f);
            float dr, dg, db, sr, sg, sb;
            samplePalette(2, dr, dg, db);
            samplePalette(3, sr, sg, sb);
            r = dr + (sr - dr) * t;
            g = dg + (sg - dg) * t;
            b = db + (sb - db) * t;
        } else {
            samplePalette(3, r, g, b);  // stone
        }

        // Slope-based darkening for depth cues
        float ny = mesh.normals.empty() ? 1.0f : mesh.normals[i * 3 + 1];
        float shade = 0.7f + 0.3f * std::max(ny, 0.0f);
        mesh.colors[i * 4 + 0] = r * shade;
        mesh.colors[i * 4 + 1] = g * shade;
        mesh.colors[i * 4 + 2] = b * shade;
        mesh.colors[i * 4 + 3] = 1.0f;
    }
}

// -------------------------------------------------------------------------
// Mesh building — mode-aware
// -------------------------------------------------------------------------

void TerrainManager::buildChunkMesh(ChunkEntry& entry, int cx, int cz) {
    if (entry.heightmap.empty()) return;

    int gridW = config_.chunkSizeX + 1;
    int gridH = config_.chunkSizeZ + 1;

    bromesh::MeshData mesh;

    switch (config_.meshMode) {
    default:
    case 0: {
        // --- Smooth: heightmapGrid with smooth normals ---
        mesh = bromesh::heightmapGrid(entry.heightmap.data(),
                                      gridW, gridH, config_.cellSize);
        break;
    }
    case 1: {
        // --- Flat: heightmapGrid then per-face normals (low-poly) ---
        mesh = bromesh::heightmapGrid(entry.heightmap.data(),
                                      gridW, gridH, config_.cellSize);
        mesh = bromesh::computeFlatNormals(mesh);
        break;
    }
    case 2: {
        // --- Terraced: quantize heights to steps, then flat normals ---
        float step = std::max(config_.terraceStep, 0.25f);
        std::vector<float> quantized(entry.heightmap.size());
        for (size_t i = 0; i < entry.heightmap.size(); i++) {
            quantized[i] = std::floor(entry.heightmap[i] / step) * step;
        }
        mesh = bromesh::heightmapGrid(quantized.data(),
                                      gridW, gridH, config_.cellSize);
        mesh = bromesh::computeFlatNormals(mesh);
        break;
    }
    case 3: {
        // --- Blocky: convert heightmap to voxel grid, greedy mesh ---
        int sizeX = config_.chunkSizeX;
        int sizeZ = config_.chunkSizeZ;

        // Determine voxel grid height from the max height in this chunk.
        float maxH = 0.0f;
        for (size_t i = 0; i < entry.heightmap.size(); i++)
            maxH = std::max(maxH, entry.heightmap[i]);
        int sizeY = std::max(static_cast<int>(maxH) + 2, 4);

        bromesh::VoxelChunk voxels(sizeX, sizeY, sizeZ, config_.cellSize);
        voxels.fill(0);

        // Fill voxel columns from heightmap (sample at grid interior points)
        for (int z = 0; z < sizeZ; z++) {
            for (int x = 0; x < sizeX; x++) {
                float fh = entry.heightmap[z * gridW + x];
                int h = static_cast<int>(fh);
                if (h < 1) h = 1;
                if (h >= sizeY) h = sizeY - 1;

                float seaF = static_cast<float>(config_.seaLevel);
                for (int y = 0; y <= h && y < sizeY; y++) {
                    uint8_t mat;
                    if (y == 0)          mat = 4;  // bedrock
                    else if (y == h)     mat = (h <= (int)seaF) ? 5 : 1;  // sand/grass
                    else if (y >= h - 3) mat = (h <= (int)seaF) ? 5 : 2;  // sand/dirt
                    else                 mat = 3;  // stone
                    voxels.setVoxel(x, y, z, mat);
                }
            }
        }
        voxels.markDirty();

        mesh = voxels.buildMesh(
            config_.palette.empty() ? nullptr : config_.palette.data(),
            static_cast<int>(config_.palette.size() / 4));

        // Blocky mesh is positioned at local origin (not centered like heightmapGrid).
        if (!entry.meshNode) {
            entry.meshNode = graph_.createMesh("terrain-chunk");
            graph_.root()->addChild(entry.meshNode);
        }
        if (mesh.positions.empty()) {
            nodeToChunk_.erase(entry.meshNode);
            graph_.destroyNode(entry.meshNode);
            entry.meshNode = nullptr;
            return;
        }
        entry.meshNode->setMesh(std::move(mesh));

        // Voxel mesh is at local origin, position at chunk corner.
        float wx = cx * sizeX * config_.cellSize;
        float wz = cz * sizeZ * config_.cellSize;
        entry.meshNode->setPosition({wx, 0.0f, wz});
        nodeToChunk_[entry.meshNode] = {cx, cz};
        return;  // blocky has its own positioning; skip the common path below
    }
    }

    // --- Common path for heightmap-based modes (0, 1, 2) ---
    if (mesh.positions.empty()) return;

    colorizeByHeight(mesh);

    if (!entry.meshNode) {
        entry.meshNode = graph_.createMesh("terrain-chunk");
        graph_.root()->addChild(entry.meshNode);
    }

    entry.meshNode->setMesh(std::move(mesh));

    // heightmapGrid is centered at origin. Position node at chunk center.
    float chunkW = config_.chunkSizeX * config_.cellSize;
    float chunkD = config_.chunkSizeZ * config_.cellSize;
    entry.meshNode->setPosition({
        cx * chunkW + chunkW * 0.5f,
        0.0f,
        cz * chunkD + chunkD * 0.5f
    });

    nodeToChunk_[entry.meshNode] = {cx, cz};
}

// -------------------------------------------------------------------------
// Chunk loading / unloading
// -------------------------------------------------------------------------

void TerrainManager::loadChunk(int cx, int cz) {
    ChunkCoord coord{cx, cz};
    if (chunks_.count(coord)) return;

    auto& entry = chunks_[coord];
    generateHeightmap(entry, cx, cz);
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

    // Determine material from heightmap at the hit point.
    auto chunkIt = chunks_.find(hitChunk);
    if (chunkIt != chunks_.end()) {
        int gridW = config_.chunkSizeX + 1;
        int gridH = config_.chunkSizeZ + 1;
        if (lx >= 0 && lx < gridW && lz >= 0 && lz < gridH) {
            float h = chunkIt->second.heightmap[lz * gridW + lx];
            float seaF = static_cast<float>(config_.seaLevel);
            if (h <= seaF) result.material = 5;       // sand
            else if (h <= seaF + config_.heightAmplitude * 0.6f) result.material = 1; // grass
            else if (h <= seaF + config_.heightAmplitude * 0.85f) result.material = 2; // dirt
            else result.material = 3;                  // stone
        }
    }

    return result;
}

// -------------------------------------------------------------------------
// Voxel edits
// -------------------------------------------------------------------------

bool TerrainManager::setVoxel(float wx, float wy, float wz, uint8_t material) {
    // Heightmap sculpting: raise or lower terrain at (wx, wz).
    // material == 0 → lower by 1, material > 0 → raise by 1.
    ChunkCoord coord;
    int lx, ly, lz;
    worldToLocal(wx, wy, wz, coord, lx, ly, lz);

    auto it = chunks_.find(coord);
    if (it == chunks_.end()) return false;

    int gridW = config_.chunkSizeX + 1;
    int gridH = config_.chunkSizeZ + 1;
    if (lx < 0 || lx >= gridW || lz < 0 || lz >= gridH) return false;

    auto& hmap = it->second.heightmap;
    float delta = (material == 0) ? -1.0f : 1.0f;
    hmap[lz * gridW + lx] += delta;
    it->second.dirty_ = true;
    return true;
}

uint8_t TerrainManager::getVoxel(float wx, float wy, float wz) const {
    // Return a material ID based on the heightmap height at (wx, wz).
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

void TerrainManager::rebuildDirty() {
    for (auto& [coord, entry] : chunks_) {
        if (entry.dirty_) {
            buildChunkMesh(entry, coord.x, coord.z);
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
