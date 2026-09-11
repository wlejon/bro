// =============================================================================
// bro_clipmap_c_abi.h — Pure C-ABI declarations for bro.clipmap
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_CLIPMAP_C_ABI_H
#define BRO_CLIPMAP_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.clipmap.ClipmapTerrain ---
void* bro_ClipmapTerrain_create(void);
void  bro_ClipmapTerrain_destroy(void* self);
void* bro_ClipmapTerrain_get_node(void* self);
int32_t bro_ClipmapTerrain_get_levels(void* self);
int32_t bro_ClipmapTerrain_get_resolution(void* self);
double bro_ClipmapTerrain_get_cellSize(void* self);
int32_t bro_ClipmapTerrain_get_layerCount(void* self);
int32_t bro_ClipmapTerrain_get_triangleCount(void* self);
int32_t bro_ClipmapTerrain_get_vertexCount(void* self);
double bro_ClipmapTerrain_get_farDistance(void* self);
double bro_ClipmapTerrain_get_cellScale(void* self);
double bro_ClipmapTerrain_get_planetRadius(void* self);
void* bro_ClipmapTerrain_setHeightLayer(void* self, int32_t index, void* desc);
void* bro_ClipmapTerrain_setSnowLine(void* self, double m);
void* bro_ClipmapTerrain_setChartCenter(void* self, double x, double z);
void* bro_ClipmapTerrain_setDetail(void* self, void* desc);
void* bro_ClipmapTerrain_setMaterials(void* self, void* desc);
void* bro_ClipmapTerrain_setForest(void* self, void* desc);
void* bro_ClipmapTerrain_setSurfaceLayer(void* self, void* indexOrDesc, void* desc);
void* bro_ClipmapTerrain_update(void* self, double camX, double camY, double camZ);
const char* bro_ClipmapTerrain_shaderSource(void* self, const char* stage);
double bro_ClipmapTerrain_elevationAt(void* self, double x, double z);
double bro_ClipmapTerrain_renderedElevationAt(void* self, double x, double z);
double bro_ClipmapTerrain_coverageDistance(void* self, double eyeAboveSeaLevel);
double bro_ClipmapTerrain_horizonDistance(void* self, double eyeAboveSeaLevel);
void bro_ClipmapTerrain_destroy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_CLIPMAP_C_ABI_H
