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

using bromath::Vec3;
using bromath::Quat;
using bromath::Mat4;

// -------------------------------------------------------------------------
// NoiseState — wraps FastNoise2 node tree
// -------------------------------------------------------------------------

struct TerrainManager::NoiseState {
    FastNoise::SmartNode<> node;
    FastNoise::SmartNode<> continentNode;  // large-scale amplitude modulation
    FastNoise::SmartNode<> mountainNode;   // enormous mountain pass
    float maxAmplitude = 1.0f;
    float mountainMaxAmplitude = 1.0f;

    void build(const TerrainConfig& cfg) {
        auto simplex = FastNoise::New<FastNoise::Simplex>();
        auto fbm = FastNoise::New<FastNoise::FractalFBm>();
        fbm->SetSource(simplex);
        fbm->SetOctaveCount(cfg.noiseOctaves);
        fbm->SetGain(cfg.noiseGain);
        fbm->SetLacunarity(cfg.noiseLacunarity);
        node = fbm;

        float amp = 0.0f;
        float g = 1.0f;
        for (int i = 0; i < cfg.noiseOctaves; i++) {
            amp += g;
            g *= cfg.noiseGain;
        }
        maxAmplitude = std::max(amp, 1.0f);

        // Continental noise: low-frequency simplex for regional variation
        if (cfg.continentFrequency > 0.0f) {
            auto csimplex = FastNoise::New<FastNoise::Simplex>();
            auto cfbm = FastNoise::New<FastNoise::FractalFBm>();
            cfbm->SetSource(csimplex);
            cfbm->SetOctaveCount(3);
            cfbm->SetGain(0.5f);
            cfbm->SetLacunarity(2.0f);
            continentNode = cfbm;
        } else {
            continentNode = nullptr;
        }

        // Mountain pass: enormous low-frequency features
        if (cfg.mountainFrequency > 0.0f) {
            auto msimplex = FastNoise::New<FastNoise::Simplex>();
            auto mfbm = FastNoise::New<FastNoise::FractalFBm>();
            mfbm->SetSource(msimplex);
            mfbm->SetOctaveCount(cfg.mountainOctaves);
            mfbm->SetGain(0.5f);
            mfbm->SetLacunarity(2.0f);
            mountainNode = mfbm;

            float mamp = 0.0f;
            float mg = 1.0f;
            for (int i = 0; i < cfg.mountainOctaves; i++) {
                mamp += mg;
                mg *= 0.5f;
            }
            mountainMaxAmplitude = std::max(mamp, 1.0f);
        } else {
            mountainNode = nullptr;
        }
    }
};

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
// Heightmap generation (noise → height values, LOD-aware)
// -------------------------------------------------------------------------

void TerrainManager::generateHeightmap(ChunkEntry& entry, int cx, int cz, int lod) {
    int gridW = config_.chunkSizeX + 1;
    int gridH = config_.chunkSizeZ + 1;
    int paddedW = gridW + 2;
    int paddedH = gridH + 2;
    size_t count = static_cast<size_t>(gridW) * gridH;
    size_t paddedCount = static_cast<size_t>(paddedW) * paddedH;
    entry.heightmap.resize(count);
    entry.heightmapPadded.resize(paddedCount);

    float effCellSize = lodCellSize(lod);

    // The interior grid is strictly derived from the padded one: row z is the
    // padded row z+1, offset one column in. Both the provider path and the noise
    // path below end with this.
    auto copyInterior = [&]() {
        for (int z = 0; z < gridH; z++) {
            std::memcpy(entry.heightmap.data() + static_cast<size_t>(z) * gridW,
                        entry.heightmapPadded.data()
                            + static_cast<size_t>(z + 1) * paddedW + 1,
                        sizeof(float) * gridW);
        }
    };

    // An external height source pre-empts the built-in generator entirely.
    // Returning false falls through to the noise below, so a provider can serve
    // only the chunks it has data for.
    if (heightSource_) {
        const float worldX0 =
            config_.origin.x + (static_cast<float>(cx) * config_.chunkSizeX - 1.0f) * effCellSize;
        const float worldZ0 =
            config_.origin.z + (static_cast<float>(cz) * config_.chunkSizeZ - 1.0f) * effCellSize;
        if (heightSource_(cx, cz, lod, entry.heightmapPadded.data(),
                          paddedW, paddedH, effCellSize, worldX0, worldZ0)) {
            copyInterior();
            return;
        }
    }

    // Sample the noise fields over a 1-voxel-wider grid on every side. The
    // outer ring is shared with the neighbouring chunks' boundary rows, which
    // lets heightmapGrid and greedyMesh produce seam-free normals and faces
    // at chunk edges.
    //
    // Each noise field has its own world-space step, so the skirt offset is
    // field-specific (shift the origin back by one step).
    float step = effCellSize * config_.noiseFrequency;
    float worldOffX = cx * config_.chunkSizeX * step - step;
    float worldOffZ = cz * config_.chunkSizeZ * step - step;

    noise_->node->GenUniformGrid2D(entry.heightmapPadded.data(),
                                   worldOffX, worldOffZ,
                                   paddedW, paddedH,
                                   step, step,
                                   config_.seed);

    // Continental noise — same world positions, much lower frequency
    std::vector<float> continent;
    if (noise_->continentNode) {
        continent.resize(paddedCount);
        float cstep = effCellSize * config_.continentFrequency;
        float cwOffX = cx * config_.chunkSizeX * cstep - cstep;
        float cwOffZ = cz * config_.chunkSizeZ * cstep - cstep;
        noise_->continentNode->GenUniformGrid2D(continent.data(),
                                                cwOffX, cwOffZ,
                                                paddedW, paddedH,
                                                cstep, cstep,
                                                config_.seed + 7777);
    }

    // Mountain pass — enormous low-frequency terrain features
    std::vector<float> mountain;
    if (noise_->mountainNode) {
        mountain.resize(paddedCount);
        float mstep = effCellSize * config_.mountainFrequency;
        float mwOffX = cx * config_.chunkSizeX * mstep - mstep;
        float mwOffZ = cz * config_.chunkSizeZ * mstep - mstep;
        noise_->mountainNode->GenUniformGrid2D(mountain.data(),
                                               mwOffX, mwOffZ,
                                               paddedW, paddedH,
                                               mstep, mstep,
                                               config_.seed + 55555);
    }

    float invAmp = 1.0f / noise_->maxAmplitude;
    float mInvAmp = noise_->mountainNode
        ? (1.0f / noise_->mountainMaxAmplitude) : 1.0f;
    float cMin = config_.continentMin;
    float cMax = config_.continentMax;

    for (size_t i = 0; i < paddedCount; i++) {
        float raw = entry.heightmapPadded[i];
        // Normalize but don't clamp — allow full height range
        float t = (raw * invAmp + 1.0f) * 0.5f;

        // Continental modulation: scale amplitude regionally
        float ampScale = 1.0f;
        if (!continent.empty()) {
            float cn = (continent[i] * 0.5f + 0.5f);
            cn = std::clamp(cn, 0.0f, 1.0f);
            ampScale = cMin + cn * (cMax - cMin);
        }

        float h = config_.baseHeight
            + (t - 0.5f) * 2.0f * config_.heightAmplitude * ampScale;

        // Add enormous mountain features, gated by continental noise
        // Mountains only appear in "mountain regions" (high ampScale)
        if (!mountain.empty()) {
            float mn = mountain[i] * mInvAmp;  // roughly [-1, 1]
            float ridge = 1.0f - std::abs(mn);
            // Fade mountains based on continental influence:
            // ampScale ranges from continentMin to continentMax
            // Normalize to [0,1] then threshold so only high regions get mountains
            float mGate = (ampScale - cMin) / std::max(cMax - cMin, 0.01f);
            mGate = std::clamp(mGate, 0.0f, 1.0f);
            mGate = mGate * mGate;  // sharpen: only strong continental regions
            h += ridge * config_.mountainAmplitude * mGate;
        }

        entry.heightmapPadded[i] = h;
    }

    // Copy the interior region into the plain heightmap for gameplay queries.
    copyInterior();
}

// -------------------------------------------------------------------------
// Curvature — applied to mesh vertices (not heightmap) so colors stay correct
// -------------------------------------------------------------------------

// Map a flat-plane point onto the sphere surface.
// Sphere center is at (0, -R, 0), so the surface at the "north pole" is the origin.
// Returns world-space position with sphere center offset applied.
Vec3 TerrainManager::sphereAnchor(float flatX, float flatZ) const {
    float R = config_.planetRadius;
    float lon = flatX / R;
    float lat = flatZ / R;
    Quat q = bromath::qmul(bromath::qaxisAngle({0, 0, 1}, -lon), bromath::qaxisAngle({1, 0, 0}, lat));
    Vec3 p = bromath::qrotate(q, {0, R, 0});
    return {p.x, p.y - R, p.z};
}

void TerrainManager::applyCurvatureToMesh(bromesh::MeshData& mesh,
                                           float chunkCenterX, float chunkCenterZ) const {
    if (config_.planetRadius <= 0.0f) return;
    float R = config_.planetRadius;
    size_t vertCount = mesh.vertexCount();

    Vec3 anchor = sphereAnchor(chunkCenterX, chunkCenterZ);

    for (size_t i = 0; i < vertCount; i++) {
        float localX = mesh.positions[i * 3 + 0];
        float h      = mesh.positions[i * 3 + 1];
        float localZ = mesh.positions[i * 3 + 2];

        // Map each vertex onto the sphere and express relative to anchor
        float lon = (chunkCenterX + localX) / R;
        float lat = (chunkCenterZ + localZ) / R;
        Quat q = bromath::qmul(bromath::qaxisAngle({0, 0, 1}, -lon), bromath::qaxisAngle({1, 0, 0}, lat));
        Vec3 spherePos = bromath::qrotate(q, {0, R + h, 0});

        mesh.positions[i * 3 + 0] = spherePos.x - anchor.x;
        mesh.positions[i * 3 + 1] = (spherePos.y - R) - anchor.y;
        mesh.positions[i * 3 + 2] = spherePos.z - anchor.z;
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
            samplePalette(5, r, g, b);
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
            samplePalette(3, r, g, b);
        }

        float ny = mesh.normals.empty() ? 1.0f : mesh.normals[i * 3 + 1];
        float shade = 0.7f + 0.3f * std::max(ny, 0.0f);
        mesh.colors[i * 4 + 0] = r * shade;
        mesh.colors[i * 4 + 1] = g * shade;
        mesh.colors[i * 4 + 2] = b * shade;
        mesh.colors[i * 4 + 3] = 1.0f;
    }
}

// -------------------------------------------------------------------------
// Mesh building — mode-aware, LOD-aware
// -------------------------------------------------------------------------

void TerrainManager::buildChunkMesh(ChunkEntry& entry, int cx, int cz, int lod) {
    auto* g = graph();
    if (!g) return;   // graph reclaimed with its canvas — nothing to mesh into
    if (entry.heightmap.empty()) return;

    int gridW = config_.chunkSizeX + 1;
    int gridH = config_.chunkSizeZ + 1;
    float effCellSize = lodCellSize(lod);

    bromesh::MeshData mesh;

    int paddedW = gridW + 2;

    switch (config_.meshMode) {
    default:
    case 0: {
        mesh = bromesh::heightmapGrid(entry.heightmapPadded.data(),
                                      gridW, gridH, effCellSize, /*border=*/1);
        break;
    }
    case 1: {
        mesh = bromesh::heightmapGrid(entry.heightmapPadded.data(),
                                      gridW, gridH, effCellSize, /*border=*/1);
        mesh = bromesh::computeFlatNormals(mesh);
        break;
    }
    case 2: {
        float step = std::max(config_.terraceStep, 0.25f);
        // Scale terrace step with LOD, but cap to avoid giant flat mesas
        if (lod > 0) {
            float scale = lodCellSize(lod) / config_.cellSize;
            step *= std::sqrt(scale);  // sqrt scaling instead of linear
        }
        // Quantize the padded heightmap so the boundary skirt is quantized to
        // the same steps as the interior — critical for flat seams to line up.
        std::vector<float> quantized(entry.heightmapPadded.size());
        for (size_t i = 0; i < entry.heightmapPadded.size(); i++) {
            quantized[i] = std::floor(entry.heightmapPadded[i] / step) * step;
        }
        mesh = bromesh::heightmapGrid(quantized.data(),
                                      gridW, gridH, effCellSize, /*border=*/1);
        mesh = bromesh::computeFlatNormals(mesh);
        break;
    }
    case 3: {
        int sizeX = config_.chunkSizeX;
        int sizeZ = config_.chunkSizeZ;

        // Size Y to the tallest column across the padded grid so neighbour
        // skirt voxels (which participate in visibility) still fit.
        float maxH = 0.0f;
        for (float h : entry.heightmapPadded) maxH = std::max(maxH, h);
        int sizeY = std::max(static_cast<int>(maxH) + 2, 4);

        // Build a voxel grid with a 1-voxel X/Z halo filled from the padded
        // heightmap so greedyMesh sees the neighbour's boundary column as a
        // solid wall and suppresses the inner face — no double-sided seam.
        int paddedX = sizeX + 2;
        int paddedZ = sizeZ + 2;
        bromesh::VoxelChunk voxels(paddedX, sizeY, paddedZ, effCellSize);
        voxels.fill(0);

        float seaF = static_cast<float>(config_.seaLevel);
        for (int pz = 0; pz < paddedZ; pz++) {
            for (int px = 0; px < paddedX; px++) {
                float fh = entry.heightmapPadded[pz * paddedW + px];
                int h = static_cast<int>(fh);
                if (h < 1) h = 1;
                if (h >= sizeY) h = sizeY - 1;

                for (int y = 0; y <= h && y < sizeY; y++) {
                    uint8_t mat;
                    if (y == 0)          mat = 4;
                    else if (y == h)     mat = (h <= (int)seaF) ? 5 : 1;
                    else if (y >= h - 3) mat = (h <= (int)seaF) ? 5 : 2;
                    else                 mat = 3;
                    voxels.setVoxel(px, y, pz, mat);
                }
            }
        }
        voxels.markDirty();

        mesh = voxels.buildMesh(
            config_.palette.empty() ? nullptr : config_.palette.data(),
            static_cast<int>(config_.palette.size() / 4),
            /*borderX=*/1, /*borderY=*/0, /*borderZ=*/1);

        if (!entry.meshNode) {
            entry.meshNode = g->createMesh("terrain-chunk");
            g->root()->addChild(entry.meshNode);
        }
        if (mesh.positions.empty()) {
            nodeToChunk_.erase(entry.meshNode);
            g->destroyNode(entry.meshNode);
            entry.meshNode = nullptr;
            return;
        }
        entry.meshNode->setMesh(std::move(mesh));

        float chunkW = config_.chunkSizeX * effCellSize;
        float chunkD = config_.chunkSizeZ * effCellSize;
        entry.meshNode->setPosition({
            config_.origin.x + cx * chunkW,
            config_.origin.y,
            config_.origin.z + cz * chunkD});
        nodeToChunk_[entry.meshNode] = {cx, cz, lod};
        return;
    }
    }

    // --- Common path for heightmap-based modes (0, 1, 2) ---
    if (mesh.positions.empty()) return;

    // Colorize BEFORE curvature so colors reflect true elevation, not curved Y
    colorizeByHeight(mesh);

    // Apply curvature to mesh vertices for LOD > 0
    float chunkW = config_.chunkSizeX * effCellSize;
    float chunkD = config_.chunkSizeZ * effCellSize;
    float centerX = cx * chunkW + chunkW * 0.5f;
    float centerZ = cz * chunkD + chunkD * 0.5f;

    // Compute mesh node position (flat or on sphere), then offset by origin
    Vec3 meshPos = {config_.origin.x + centerX, config_.origin.y, config_.origin.z + centerZ};

    if (lod > 0 && config_.planetRadius > 0.0f) {
        Vec3 anchor = sphereAnchor(centerX, centerZ);
        meshPos = {config_.origin.x + anchor.x,
                   config_.origin.y + anchor.y,
                   config_.origin.z + anchor.z};

        applyCurvatureToMesh(mesh, centerX, centerZ);
    }

    if (!entry.meshNode) {
        entry.meshNode = g->createMesh("terrain-chunk");
        g->root()->addChild(entry.meshNode);
    }

    entry.meshNode->setMesh(std::move(mesh));
    entry.meshNode->setPosition(meshPos);

    if (lod == 0) {
        entry.meshNode->setDepthBias(-1.0f, -1.0f);
        entry.meshNode->setNearClipDist(0.0f);
    } else {
        // Clip this LOD's fragments where the finer LOD covers
        float finerCoverage = lodChunkWorldSize(lod - 1) * lodLoadRadius(lod - 1);
        entry.meshNode->setNearClipDist(finerCoverage * 0.9f);
    }

    nodeToChunk_[entry.meshNode] = {cx, cz, lod};
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
// Raycast (LOD 0 only)
// -------------------------------------------------------------------------

TerrainHit TerrainManager::raycast(const Vec3& origin, const Vec3& dir, float maxDist) const {
    TerrainHit result;

    Vec3 ndir = bromath::vnorm(dir);
    if (bromath::vlen2(ndir) < 1e-12f) return result;

    float closestDist = (maxDist > 0.0f) ? maxDist : 1e30f;
    const MeshNode* closestNode = nullptr;
    Vec3 closestWorldPos;
    Vec3 closestWorldNormal;

    for (auto& [coord, entry] : chunks_) {
        // Only raycast against LOD 0 (detail level)
        if (coord.lod != 0) continue;
        if (!entry.meshNode || !entry.meshNode->visible()) continue;

        const auto& md = entry.meshNode->mesh();
        if (md.positions.empty() || md.indices.empty()) continue;

        const Vec3& nodePos = entry.meshNode->position();
        const Quat& nodeRot = entry.meshNode->rotation();
        const Vec3& nodeScl = entry.meshNode->scale();

        Vec3 localOrigin = origin - nodePos;
        localOrigin = bromath::qrotate(bromath::qconjugate(nodeRot), localOrigin);
        if (nodeScl.x != 0.0f) localOrigin.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localOrigin.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localOrigin.z /= nodeScl.z;

        Vec3 localDir = bromath::qrotate(bromath::qconjugate(nodeRot), ndir);
        if (nodeScl.x != 0.0f) localDir.x /= nodeScl.x;
        if (nodeScl.y != 0.0f) localDir.y /= nodeScl.y;
        if (nodeScl.z != 0.0f) localDir.z /= nodeScl.z;

        float localDirLen = bromath::vlen(localDir);
        if (localDirLen < 1e-12f) continue;
        Vec3 localDirN = localDir * (1.0f / localDirLen);

        float scale = nodeScl.x != 0.0f ? nodeScl.x : 1.0f;
        float localMaxDist = closestDist / scale;

        const bromath::AABB3& lb = entry.meshNode->localBounds();
        float bmin[3] = { lb.min.x, lb.min.y, lb.min.z };
        float bmax[3] = { lb.max.x, lb.max.y, lb.max.z };
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

        float rayO[3] = { localOrigin.x, localOrigin.y, localOrigin.z };
        float rayD[3] = { localDirN.x, localDirN.y, localDirN.z };
        bromesh::RayHit hit = entry.meshNode->bvh().raycast(md, rayO, rayD, localMaxDist);
        if (!hit.hit) continue;

        Vec3 localHit{hit.position[0], hit.position[1], hit.position[2]};
        localHit.x *= nodeScl.x;
        localHit.y *= nodeScl.y;
        localHit.z *= nodeScl.z;
        Vec3 worldHit = bromath::qrotate(nodeRot, localHit) + nodePos;

        float worldDist = bromath::vlen(worldHit - origin);
        if (worldDist >= closestDist) continue;

        closestDist = worldDist;
        closestNode = entry.meshNode;
        closestWorldPos = worldHit;

        Vec3 localNormal{hit.normal[0], hit.normal[1], hit.normal[2]};
        closestWorldNormal = bromath::vnorm(bromath::qrotate(nodeRot, localNormal));
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

    auto it = nodeToChunk_.find(closestNode);
    if (it != nodeToChunk_.end()) {
        result.chunk = it->second;
    }

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

    auto chunkIt = chunks_.find(hitChunk);
    if (chunkIt != chunks_.end()) {
        int gridW = config_.chunkSizeX + 1;
        int gridH = config_.chunkSizeZ + 1;
        if (lx >= 0 && lx < gridW && lz >= 0 && lz < gridH) {
            float h = chunkIt->second.heightmap[lz * gridW + lx];
            float seaF = static_cast<float>(config_.seaLevel);
            if (h <= seaF) result.material = 5;
            else if (h <= seaF + config_.heightAmplitude * 0.6f) result.material = 1;
            else if (h <= seaF + config_.heightAmplitude * 0.85f) result.material = 2;
            else result.material = 3;
        }
    }

    return result;
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
