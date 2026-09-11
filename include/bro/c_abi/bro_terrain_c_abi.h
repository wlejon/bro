// =============================================================================
// bro_terrain_c_abi.h — Pure C-ABI declarations for bro.terrain
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_TERRAIN_C_ABI_H
#define BRO_TERRAIN_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.terrain.Terrain ---
void* bro_Terrain_create(void);
void  bro_Terrain_destroy(void* self);
int32_t bro_Terrain_get_chunkCount(void* self);
int32_t bro_Terrain_get_triangleCount(void* self);
int32_t bro_Terrain_get_vertexCount(void* self);
double bro_Terrain_get_farDistance(void* self);
double bro_Terrain_get_planetRadius(void* self);
void* bro_Terrain_get_origin(void* self);
int32_t bro_Terrain_update(void* self, double x, double y, double z);
void* bro_Terrain_raycast(void* self, void* origin, void* direction, double maxDist);
bool bro_Terrain_setVoxel(void* self, double wx, double wy, double wz, int32_t material);
int32_t bro_Terrain_getVoxel(void* self, double wx, double wy, double wz);
void bro_Terrain_rebuild(void* self);
void bro_Terrain_configure(void* self, void* config);
void bro_Terrain_invalidateRegion(void* self, double x0, double z0, double x1, double z1);
void bro_Terrain_setHeightSource(void* self, void* fn);
double bro_Terrain_heightAt(void* self, double x, double z);
void* bro_Terrain_normalAt(void* self, double x, double z);
double bro_Terrain_elevation(void* self, double x, double z);
void bro_Terrain_splat(void* self, double x, double z, double radius, int32_t layer);
int32_t bro_Terrain_get_layers(void* self);
void bro_Terrain_destroy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_TERRAIN_C_ABI_H
