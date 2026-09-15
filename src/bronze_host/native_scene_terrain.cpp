// native_scene_terrain.cpp — TerrainManager binding and C entry points.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/terrain/native_terrain_decl.h"
#include "scene/terrain_manager.h"
#include <cmath>

namespace bro::bronze_host {

bool registerNatives_terrain(std::string* error);

namespace {

struct TerrainRaycastSlot {
    bool hit = false;
    double distance = 0.0;
    double position[3] = {0, 0, 0};
    double normal[3] = {0, 1, 0};
    int32_t chunk[2] = {0, 0};
    int32_t voxel[3] = {0, 0, 0};
    int32_t material = 0;
};
static thread_local TerrainRaycastSlot tl_terrainRaycast;

static thread_local double tl_buf3[3];
static thread_local int32_t tl_ibuf[3];

}  // namespace

bool registerTerrainNatives(std::string* error) {
    return registerNatives_terrain(error);
}

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void bro_terrain_Terrain_dtor(void* self) {
    delete terrainCellOf(self);
}

void* bro_terrain_Terrain_ctor(void) {
    return new HostTerrainCell();
}

int32_t bro_terrain_Terrain_chunkCount_get(void* self) {
    auto* c = terrainCellOf(self);
    return (c && c->mgr()) ? c->mgr()->chunkCount() : 0;
}

int32_t bro_terrain_Terrain_triangleCount_get(void* self) {
    auto* c = terrainCellOf(self);
    return (c && c->mgr()) ? c->mgr()->totalTriangles() : 0;
}

int32_t bro_terrain_Terrain_vertexCount_get(void* self) {
    auto* c = terrainCellOf(self);
    return (c && c->mgr()) ? c->mgr()->totalVertices() : 0;
}

double bro_terrain_Terrain_farDistance_get(void* self) {
    auto* c = terrainCellOf(self);
    return (c && c->mgr()) ? c->mgr()->farDistance() : 1000.0;
}

double bro_terrain_Terrain_planetRadius_get(void* self) {
    auto* c = terrainCellOf(self);
    return (c && c->mgr()) ? c->mgr()->config().planetRadius : 0.0;
}

void bro_terrain_Terrain_origin_get(void* self, bronze_native_buffer* out) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) {
        tl_buf3[0] = 0; tl_buf3[1] = 0; tl_buf3[2] = 0;
    } else {
        const auto& o = c->mgr()->config().origin;
        tl_buf3[0] = o.x; tl_buf3[1] = o.y; tl_buf3[2] = o.z;
    }
    copyBuffer(tl_buf3, 3, out);
}

int32_t bro_terrain_Terrain_update(void* self, double x, double y, double z) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) return 0;
    return c->mgr()->update(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
}

bool bro_terrain_Terrain_raycast(void* self, const double* origin, uint32_t origin_len,
                                const double* direction, uint32_t direction_len,
                                bool maxDist_given, double maxDist) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr() || !origin || origin_len < 3 || !direction || direction_len < 3) {
        tl_terrainRaycast.hit = false;
        return false;
    }
    bromath::Vec3 o{static_cast<float>(origin[0]), static_cast<float>(origin[1]), static_cast<float>(origin[2])};
    bromath::Vec3 d{static_cast<float>(direction[0]), static_cast<float>(direction[1]), static_cast<float>(direction[2])};
    float dist = maxDist_given ? static_cast<float>(maxDist) : 1000.0f;
    auto hit = c->mgr()->raycast(o, d, dist);
    if (!hit.hit) {
        tl_terrainRaycast.hit = false;
        return false;
    }
    tl_terrainRaycast.hit = true;
    tl_terrainRaycast.distance = hit.distance;
    tl_terrainRaycast.position[0] = hit.worldPos[0];
    tl_terrainRaycast.position[1] = hit.worldPos[1];
    tl_terrainRaycast.position[2] = hit.worldPos[2];
    tl_terrainRaycast.normal[0] = hit.normal[0];
    tl_terrainRaycast.normal[1] = hit.normal[1];
    tl_terrainRaycast.normal[2] = hit.normal[2];
    tl_terrainRaycast.chunk[0] = hit.chunk.x;
    tl_terrainRaycast.chunk[1] = hit.chunk.z;
    tl_terrainRaycast.voxel[0] = hit.localX;
    tl_terrainRaycast.voxel[1] = hit.localY;
    tl_terrainRaycast.voxel[2] = hit.localZ;
    tl_terrainRaycast.material = hit.material;
    return true;
}

bool bro_terrain_Terrain_raycast_hit(void) {
    return tl_terrainRaycast.hit;
}

double bro_terrain_Terrain_raycast_distance(void) {
    return tl_terrainRaycast.distance;
}

void bro_terrain_Terrain_raycast_position(bronze_native_buffer* out) {
    copyBuffer(tl_terrainRaycast.position, 3, out);
}

void bro_terrain_Terrain_raycast_normal(bronze_native_buffer* out) {
    copyBuffer(tl_terrainRaycast.normal, 3, out);
}

void bro_terrain_Terrain_raycast_chunk(bronze_native_buffer* out) {
    tl_ibuf[0] = tl_terrainRaycast.chunk[0];
    tl_ibuf[1] = tl_terrainRaycast.chunk[1];
    copyBuffer(tl_ibuf, 2, out);
}

void bro_terrain_Terrain_raycast_voxel(bronze_native_buffer* out) {
    tl_ibuf[0] = tl_terrainRaycast.voxel[0];
    tl_ibuf[1] = tl_terrainRaycast.voxel[1];
    tl_ibuf[2] = tl_terrainRaycast.voxel[2];
    copyBuffer(tl_ibuf, 3, out);
}

int32_t bro_terrain_Terrain_raycast_material(void) {
    return tl_terrainRaycast.material;
}

bool bro_terrain_Terrain_setVoxel(void* self, double wx, double wy, double wz, int32_t material) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) return false;
    return c->mgr()->setVoxel(static_cast<float>(wx), static_cast<float>(wy), static_cast<float>(wz), static_cast<uint8_t>(material));
}

int32_t bro_terrain_Terrain_getVoxel(void* self, double wx, double wy, double wz) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) return 0;
    return c->mgr()->getVoxel(static_cast<float>(wx), static_cast<float>(wy), static_cast<float>(wz));
}

void bro_terrain_Terrain_rebuild(void* self) {
    auto* c = terrainCellOf(self);
    if (c && c->mgr()) c->mgr()->rebuildDirty();
}

void bro_terrain_Terrain_configure(void* self, const int32_t* config_chunkSize, uint32_t config_chunkSize_len,
                                  bool config_cellSize_given, double config_cellSize,
                                  bool config_loadRadius_given, int32_t config_loadRadius,
                                  bool config_unloadRadius_given, int32_t config_unloadRadius,
                                  bool config_maxLoadsPerUpdate_given, int32_t config_maxLoadsPerUpdate,
                                  bool config_seed_given, int32_t config_seed,
                                  bool config_noise_frequency_given, double config_noise_frequency,
                                  bool config_noise_octaves_given, int32_t config_noise_octaves,
                                  bool config_noise_gain_given, double config_noise_gain,
                                  bool config_noise_lacunarity_given, double config_noise_lacunarity,
                                  bool config_baseHeight_given, int32_t config_baseHeight,
                                  bool config_heightAmplitude_given, int32_t config_heightAmplitude,
                                  bool config_seaLevel_given, int32_t config_seaLevel,
                                  bool config_meshMode_given, int32_t config_meshMode,
                                  bool config_terraceStep_given, double config_terraceStep,
                                  bool config_continentFrequency_given, double config_continentFrequency,
                                  bool config_continentMin_given, double config_continentMin,
                                  bool config_continentMax_given, double config_continentMax,
                                  bool config_mountainFrequency_given, double config_mountainFrequency,
                                  bool config_mountainAmplitude_given, double config_mountainAmplitude,
                                  bool config_mountainOctaves_given, int32_t config_mountainOctaves,
                                  bool config_lodLevels_given, int32_t config_lodLevels,
                                  bool config_lodScaleFactor_given, int32_t config_lodScaleFactor,
                                  bool config_planetRadius_given, double config_planetRadius,
                                  const double* config_origin, uint32_t config_origin_len,
                                  const float* config_palette, uint32_t config_palette_len) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) return;
    scene::TerrainConfig cfg;
    if (config_chunkSize && config_chunkSize_len >= 3) {
        cfg.chunkSizeX = config_chunkSize[0];
        cfg.chunkSizeY = config_chunkSize[1];
        cfg.chunkSizeZ = config_chunkSize[2];
    }
    if (config_cellSize_given) cfg.cellSize = static_cast<float>(config_cellSize);
    if (config_loadRadius_given) cfg.loadRadius = config_loadRadius;
    if (config_unloadRadius_given) cfg.unloadRadius = config_unloadRadius;
    if (config_maxLoadsPerUpdate_given) cfg.maxLoadsPerUpdate = config_maxLoadsPerUpdate;
    if (config_seed_given) cfg.seed = config_seed;
    if (config_noise_frequency_given) cfg.noiseFrequency = static_cast<float>(config_noise_frequency);
    if (config_noise_octaves_given) cfg.noiseOctaves = config_noise_octaves;
    if (config_noise_gain_given) cfg.noiseGain = static_cast<float>(config_noise_gain);
    if (config_noise_lacunarity_given) cfg.noiseLacunarity = static_cast<float>(config_noise_lacunarity);
    if (config_baseHeight_given) cfg.baseHeight = config_baseHeight;
    if (config_heightAmplitude_given) cfg.heightAmplitude = config_heightAmplitude;
    if (config_seaLevel_given) cfg.seaLevel = config_seaLevel;
    if (config_meshMode_given) cfg.meshMode = config_meshMode;
    if (config_terraceStep_given) cfg.terraceStep = static_cast<float>(config_terraceStep);
    if (config_continentFrequency_given) cfg.continentFrequency = static_cast<float>(config_continentFrequency);
    if (config_continentMin_given) cfg.continentMin = static_cast<float>(config_continentMin);
    if (config_continentMax_given) cfg.continentMax = static_cast<float>(config_continentMax);
    if (config_mountainFrequency_given) cfg.mountainFrequency = static_cast<float>(config_mountainFrequency);
    if (config_mountainAmplitude_given) cfg.mountainAmplitude = static_cast<float>(config_mountainAmplitude);
    if (config_mountainOctaves_given) cfg.mountainOctaves = config_mountainOctaves;
    if (config_lodLevels_given) cfg.lodLevelCount = config_lodLevels;
    if (config_lodScaleFactor_given) cfg.lodScaleFactor = config_lodScaleFactor;
    if (config_planetRadius_given) cfg.planetRadius = static_cast<float>(config_planetRadius);
    if (config_origin && config_origin_len >= 3) {
        cfg.origin = {static_cast<float>(config_origin[0]), static_cast<float>(config_origin[1]), static_cast<float>(config_origin[2])};
    }
    if (config_palette && config_palette_len > 0) {
        cfg.palette.assign(config_palette, config_palette + config_palette_len);
    }
    c->mgr()->configure(cfg);
}

void bro_terrain_Terrain_invalidateRegion(void* self, double x0, double z0, double x1, double z1) {
    auto* c = terrainCellOf(self);
    if (c && c->mgr()) {
        c->mgr()->invalidateRegion(static_cast<float>(x0), static_cast<float>(z0), static_cast<float>(x1), static_cast<float>(z1));
    }
}

void bro_terrain_Terrain_setHeightSource(void* self, uint64_t /*fn*/) {
    // Height source callback placeholder
}

double bro_terrain_Terrain_heightAt(void* self, double x, double z) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) return 0.0;
    const bromath::Vec3 origin{static_cast<float>(x), 1000.0f, static_cast<float>(z)};
    const bromath::Vec3 dir{0.0f, -1.0f, 0.0f};
    auto hit = c->mgr()->raycast(origin, dir, 2000.0f);
    return hit.hit ? hit.worldPos[1] : 0.0;
}

void bro_terrain_Terrain_normalAt(void* self, double x, double z, bronze_native_buffer* out) {
    auto* c = terrainCellOf(self);
    if (!c || !c->mgr()) {
        tl_buf3[0] = 0.0; tl_buf3[1] = 1.0; tl_buf3[2] = 0.0;
    } else {
        const bromath::Vec3 origin{static_cast<float>(x), 1000.0f, static_cast<float>(z)};
        const bromath::Vec3 dir{0.0f, -1.0f, 0.0f};
        auto hit = c->mgr()->raycast(origin, dir, 2000.0f);
        if (hit.hit) {
            tl_buf3[0] = hit.normal[0];
            tl_buf3[1] = hit.normal[1];
            tl_buf3[2] = hit.normal[2];
        } else {
            tl_buf3[0] = 0.0; tl_buf3[1] = 1.0; tl_buf3[2] = 0.0;
        }
    }
    copyBuffer(tl_buf3, 3, out);
}

double bro_terrain_Terrain_elevation(void* self, double x, double z) {
    return bro_terrain_Terrain_heightAt(self, x, z);
}

void bro_terrain_Terrain_splat(void* self, double x, double z, double radius, int32_t layer) {
    auto* c = terrainCellOf(self);
    if (c && c->mgr()) {
        float r = static_cast<float>(radius);
        for (float dx = -r; dx <= r; dx += 1.0f) {
            for (float dz = -r; dz <= r; dz += 1.0f) {
                if (dx*dx + dz*dz <= r*r) {
                    c->mgr()->setVoxel(static_cast<float>(x + dx), 0.0f, static_cast<float>(z + dz), static_cast<uint8_t>(layer));
                }
            }
        }
    }
}

int32_t bro_terrain_Terrain_layers_get(void* self) {
    return 4;
}

void bro_terrain_Terrain_destroy(void* self) {
    auto* c = terrainCellOf(self);
    if (c && c->manager) {
        c->manager->clear();
        c->manager.reset();
    }
}

}  // extern "C"
