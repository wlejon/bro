// =============================================================================
// bro_tile_world_c_abi.h — Pure C-ABI declarations for bro.tile_world
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_TILE_WORLD_C_ABI_H
#define BRO_TILE_WORLD_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.tile_world.TileWorld ---
void* bro_TileWorld_create(void);
void  bro_TileWorld_destroy(void* self);
void* bro_TileWorld_get_node(void* self);
int32_t bro_TileWorld_get_width(void* self);
int32_t bro_TileWorld_get_height(void* self);
int32_t bro_TileWorld_get_chunkCount(void* self);
int32_t bro_TileWorld_get_chunks(void* self);
bool bro_TileWorld_get_paging(void* self);
void bro_TileWorld_set_paging(void* self, bool val);
int32_t bro_TileWorld_get_vertexCount(void* self);
int32_t bro_TileWorld_get_triangleCount(void* self);
int32_t bro_TileWorld_update(void* self, double camX, double camY, double camZ);
void bro_TileWorld_setTile(void* self, int32_t layer, int32_t x, int32_t y, int32_t tileId);
int32_t bro_TileWorld_getTile(void* self, int32_t layer, int32_t x, int32_t y);
void bro_TileWorld_fillRect(void* self, int32_t layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t tileId);
void bro_TileWorld_clearLayer(void* self, int32_t layer);
void* bro_TileWorld_pickTile(void* self, double worldX, double worldZ);
void* bro_TileWorld_findPath(void* self, int32_t startX, int32_t startY, int32_t endX, int32_t endY, void* opts);
void* bro_TileWorld_computeRegions(void* self, int32_t layer);
void bro_TileWorld_applyAutotile(void* self, int32_t layer, void* rules);
void* bro_TileWorld_extractVoxelMesh(void* self, void* opts);
void bro_TileWorld_setOrigin(void* self, double x, double y, double z);
bool bro_TileWorld_advance(void* self, double dtMs);
void bro_TileWorld_addObjectKind(void* self, int32_t kindId, void* spec);
void bro_TileWorld_addObject(void* self, int32_t kindId, double x, double y, double z);
void bro_TileWorld_clearObjects(void* self, int32_t kindId);
int32_t bro_TileWorld_objectCount(void* self, int32_t kind);
void bro_TileWorld_rebuildObjects(void* self);
void bro_TileWorld_rebuild(void* self);
void bro_TileWorld_rebuildAll(void* self);
void bro_TileWorld_configure(void* self, void* cfg);
void* bro_TileWorld_save(void* self);
bool bro_TileWorld_load(void* self, void* data);
void bro_TileWorld_destroy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_TILE_WORLD_C_ABI_H
