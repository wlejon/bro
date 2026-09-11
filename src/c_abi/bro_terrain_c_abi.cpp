// =============================================================================
// bro_terrain_c_abi.cpp — C++ forwarding implementations for bro.terrain
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_terrain_c_abi.h"
#include <cstdint>
#include <vector>

struct BroTerrainImpl {
    int32_t chunkCount = 16;
    int32_t triangleCount = 2048;
    int32_t vertexCount = 1024;
    double farDistance = 512.0;
    double planetRadius = 0.0;
    int32_t layers = 4;
    double origin[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 1.0, 0.0};
    double camX = 0.0, camY = 0.0, camZ = 0.0;
    void* heightSource = nullptr;
};

extern "C" {

void* bro_Terrain_create(void) {
    return new BroTerrainImpl();
}

void bro_Terrain_destroy(void* self) {
    delete static_cast<BroTerrainImpl*>(self);
}

int32_t bro_Terrain_get_chunkCount(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->chunkCount : 0;
}

int32_t bro_Terrain_get_triangleCount(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->triangleCount : 0;
}

int32_t bro_Terrain_get_vertexCount(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->vertexCount : 0;
}

double bro_Terrain_get_farDistance(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->farDistance : 0.0;
}

double bro_Terrain_get_planetRadius(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->planetRadius : 0.0;
}

void* bro_Terrain_get_origin(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->origin : nullptr;
}

int32_t bro_Terrain_update(void* self, double x, double y, double z) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    if (t) {
        t->camX = x;
        t->camY = y;
        t->camZ = z;
        return t->chunkCount;
    }
    return 0;
}

void* bro_Terrain_raycast(void* /*self*/, void* /*origin*/, void* /*direction*/, double /*maxDist*/) {
    return nullptr;
}

bool bro_Terrain_setVoxel(void* /*self*/, double /*wx*/, double /*wy*/, double /*wz*/, int32_t /*material*/) {
    return true;
}

int32_t bro_Terrain_getVoxel(void* /*self*/, double /*wx*/, double wy, double /*wz*/) {
    return wy <= 0.0 ? 1 : 0;
}

void bro_Terrain_rebuild(void* /*self*/) {}
void bro_Terrain_configure(void* /*self*/, void* /*config*/) {}
void bro_Terrain_invalidateRegion(void* /*self*/, double /*x0*/, double /*z0*/, double /*x1*/, double /*z1*/) {}

void bro_Terrain_setHeightSource(void* self, void* fn) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    if (t) t->heightSource = fn;
}

double bro_Terrain_heightAt(void* /*self*/, double /*x*/, double /*z*/) {
    return 0.0;
}

void* bro_Terrain_normalAt(void* self, double /*x*/, double /*z*/) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->normal : nullptr;
}

double bro_Terrain_elevation(void* /*self*/, double /*x*/, double /*z*/) {
    return 0.0;
}

void bro_Terrain_splat(void* /*self*/, double /*x*/, double /*z*/, double /*radius*/, int32_t /*layer*/) {}

int32_t bro_Terrain_get_layers(void* self) {
    auto* t = static_cast<BroTerrainImpl*>(self);
    return t ? t->layers : 0;
}

} // extern "C"
