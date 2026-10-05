#include "scene/terrain_manager.h"
#include "scene/terrain_manager_internal.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"

#include <bromesh/primitives/primitives.h>
#include <bromesh/manipulation/normals.h>
#include <bromesh/voxel/voxel_chunk.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene {

using bromath::Vec3;
using bromath::Quat;

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

} // namespace bro::scene
