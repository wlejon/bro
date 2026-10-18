// native_tile_world_decl.h — the C entry points native_tile_world_register.cpp registers, one
// prototype per native. Hand-maintained: a native is added or changed here, in the
// register file, in its JS wrapper and in its body together. The bodies are
// hand-written in bro against the engine, and the compiler checks them here.
//
// How each type crosses (bronze/src/abi/bronze_native_type.h):
//   double / int32_t / bool      f64 / i32 / bool
//   const char*                  str — UTF-8, valid for the call only; a RESULT must
//                                outlive the return (a per-thread scratch), and one
//                                marked JSON carries JSON of the type named in the comment
//   uint64_t                     dynamic — a callback's raw Value bits; keep it in a
//                                Persistent, never across an allocating call raw
//   (const T*, uint32_t)         a typed array: elements and count, valid for the call
//   void*                        a class handle: what the class constructor returned
//   bronze_native_buffer* out    a typed-array RESULT: the function returns void and
//                                fills *out (data, length in elements); release == NULL
//                                and the runtime copies during the call; release set
//                                ([transfer]) and the array is a view over data, with
//                                release(ctx) run when it is collected
//   bool <x>_given, T <x>        an optional value with no declared default, and
//                                whether it was passed
//
// A dictionary or list RESULT crosses as reads: the operation runs and keeps its
// result in a per-thread slot, answering nothing (or the list's length), and the
// <op>_<member> / <op>_at natives read from that slot until the next call.
//
// Gate: BRO_WITH_3D (native_tile_world_register.cpp registers nothing when it is off).
//
// Names classes of: scene — register those subsystems first.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "abi/bronze_native_type.h"  // bronze_native_buffer, owned by bronze

#ifdef __cplusplus
extern "C" {
#endif

// destructor of __bro_native.tile_world.TileWorld: frees what the constructor (or a native returning a TileWorld) allocated
void bro_tile_world_TileWorld_dtor(void* self);

// constructor of __bro_native.tile_world.TileWorld: registered so the class exists (instances come from natives returning a TileWorld); never called by the wrapper, the body may return NULL
//   registered at __bro_native.tile_world.TileWorld
void* bro_tile_world_TileWorld_ctor(void);

// bro.tile_world.TileWorld.prototype.node read
//   registered at __bro_native.tile_world.TileWorld_node_get
void* bro_tile_world_TileWorld_node_get(void* self);

// bro.tile_world.TileWorld.prototype.width read
//   registered at __bro_native.tile_world.TileWorld_width_get
int32_t bro_tile_world_TileWorld_width_get(void* self);

// bro.tile_world.TileWorld.prototype.height read
//   registered at __bro_native.tile_world.TileWorld_height_get
int32_t bro_tile_world_TileWorld_height_get(void* self);

// bro.tile_world.TileWorld.prototype.chunkCount read
//   registered at __bro_native.tile_world.TileWorld_chunkCount_get
int32_t bro_tile_world_TileWorld_chunkCount_get(void* self);

// bro.tile_world.TileWorld.prototype.chunks read
//   registered at __bro_native.tile_world.TileWorld_chunks_get
int32_t bro_tile_world_TileWorld_chunks_get(void* self);

// bro.tile_world.TileWorld.prototype.vertexCount read
//   registered at __bro_native.tile_world.TileWorld_vertexCount_get
int32_t bro_tile_world_TileWorld_vertexCount_get(void* self);

// bro.tile_world.TileWorld.prototype.triangleCount read
//   registered at __bro_native.tile_world.TileWorld_triangleCount_get
int32_t bro_tile_world_TileWorld_triangleCount_get(void* self);

// bro.tile_world.TileWorld.prototype.setTile
//   registered at __bro_native.tile_world.TileWorld_setTile
void bro_tile_world_TileWorld_setTile(void* self, int32_t layer, int32_t x, int32_t y, int32_t tileId);

// bro.tile_world.TileWorld.prototype.getTile
//   registered at __bro_native.tile_world.TileWorld_getTile
int32_t bro_tile_world_TileWorld_getTile(void* self, int32_t layer, int32_t x, int32_t y);

// bro.tile_world.TileWorld.prototype.fillRect
//   registered at __bro_native.tile_world.TileWorld_fillRect
void bro_tile_world_TileWorld_fillRect(void* self, int32_t layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t tileId);

// bro.tile_world.TileWorld.prototype.clearLayer
//   registered at __bro_native.tile_world.TileWorld_clearLayer
void bro_tile_world_TileWorld_clearLayer(void* self, int32_t layer);

// bro.tile_world.TileWorld.prototype.pickTile: runs the operation and keeps its TilePickResult result in a per-thread slot the TileWorld_pickTile_<member> reads answer from until the next call; false = null
//   registered at __bro_native.tile_world.TileWorld_pickTile
bool bro_tile_world_TileWorld_pickTile(void* self, double worldX, double worldZ);

// TilePickResult.tileX of the result TileWorld_pickTile kept
//   registered at __bro_native.tile_world.TileWorld_pickTile_tileX
int32_t bro_tile_world_TileWorld_pickTile_tileX(void);

// TilePickResult.tileY of the result TileWorld_pickTile kept
//   registered at __bro_native.tile_world.TileWorld_pickTile_tileY
int32_t bro_tile_world_TileWorld_pickTile_tileY(void);

// TilePickResult.layer of the result TileWorld_pickTile kept
//   registered at __bro_native.tile_world.TileWorld_pickTile_layer
int32_t bro_tile_world_TileWorld_pickTile_layer(void);

// TilePickResult.tileId of the result TileWorld_pickTile kept
//   registered at __bro_native.tile_world.TileWorld_pickTile_tileId
int32_t bro_tile_world_TileWorld_pickTile_tileId(void);

// TilePickResult.worldPosition of the result TileWorld_pickTile kept
//   registered at __bro_native.tile_world.TileWorld_pickTile_worldPosition
//   result: f64[] in *out; out->release == NULL, the runtime copies
void bro_tile_world_TileWorld_pickTile_worldPosition(bronze_native_buffer* out);

// bro.tile_world.TileWorld.prototype.findPath: runs the operation and keeps its TilePathResult result in a per-thread slot the TileWorld_findPath_<member> reads answer from until the next call
//   registered at __bro_native.tile_world.TileWorld_findPath
//   opts_agentRadius_given, opts_allowDiagonal_given, opts_maxSlope_given: false when the value was not passed (no declared default)
void bro_tile_world_TileWorld_findPath(void* self, int32_t startX, int32_t startY, int32_t endX, int32_t endY, bool opts_agentRadius_given, int32_t opts_agentRadius, bool opts_allowDiagonal_given, bool opts_allowDiagonal, bool opts_maxSlope_given, int32_t opts_maxSlope);

// TilePathResult.reachable of the result TileWorld_findPath kept
//   registered at __bro_native.tile_world.TileWorld_findPath_reachable
bool bro_tile_world_TileWorld_findPath_reachable(void);

// TilePathResult.path of the result TileWorld_findPath kept
//   registered at __bro_native.tile_world.TileWorld_findPath_path
//   result: JSON of sequence<sequence<long>>
const char* bro_tile_world_TileWorld_findPath_path(void);

// TilePathResult.totalCost of the result TileWorld_findPath kept
//   registered at __bro_native.tile_world.TileWorld_findPath_totalCost
double bro_tile_world_TileWorld_findPath_totalCost(void);

// bro.tile_world.TileWorld.prototype.computeRegions: runs the operation, keeps the list in a per-thread slot, and answers its length; TileWorld_computeRegions_<member>(index) reads from it
//   registered at __bro_native.tile_world.TileWorld_computeRegions
int32_t bro_tile_world_TileWorld_computeRegions(void* self, int32_t layer);

// TileRegionResult.regionId of the result TileWorld_computeRegions kept, at [index]
//   registered at __bro_native.tile_world.TileWorld_computeRegions_regionId
int32_t bro_tile_world_TileWorld_computeRegions_regionId(int32_t index);

// TileRegionResult.tiles of the result TileWorld_computeRegions kept, at [index]
//   registered at __bro_native.tile_world.TileWorld_computeRegions_tiles
//   result: JSON of sequence<sequence<long>>
const char* bro_tile_world_TileWorld_computeRegions_tiles(int32_t index);

// TileRegionResult.area of the result TileWorld_computeRegions kept, at [index]
//   registered at __bro_native.tile_world.TileWorld_computeRegions_area
int32_t bro_tile_world_TileWorld_computeRegions_area(int32_t index);

// bro.tile_world.TileWorld.prototype.setOrigin
//   registered at __bro_native.tile_world.TileWorld_setOrigin
void bro_tile_world_TileWorld_setOrigin(void* self, double x, double y, double z);

// bro.tile_world.TileWorld.prototype.advance
//   registered at __bro_native.tile_world.TileWorld_advance
bool bro_tile_world_TileWorld_advance(void* self, double dtMs);

// bro.tile_world.TileWorld.prototype.addObject
//   registered at __bro_native.tile_world.TileWorld_addObject
void bro_tile_world_TileWorld_addObject(void* self, int32_t kindId, double x, double y, double z);

// bro.tile_world.TileWorld.prototype.clearObjects
//   registered at __bro_native.tile_world.TileWorld_clearObjects
//   kindId_given: false when the value was not passed (no declared default)
void bro_tile_world_TileWorld_clearObjects(void* self, bool kindId_given, int32_t kindId);

// bro.tile_world.TileWorld.prototype.objectCount
//   registered at __bro_native.tile_world.TileWorld_objectCount
int32_t bro_tile_world_TileWorld_objectCount(void* self, int32_t kind);

// bro.tile_world.TileWorld.prototype.rebuildObjects
//   registered at __bro_native.tile_world.TileWorld_rebuildObjects
void bro_tile_world_TileWorld_rebuildObjects(void* self);

// bro.tile_world.TileWorld.prototype.rebuild
//   registered at __bro_native.tile_world.TileWorld_rebuild
void bro_tile_world_TileWorld_rebuild(void* self);

// bro.tile_world.TileWorld.prototype.rebuildAll
//   registered at __bro_native.tile_world.TileWorld_rebuildAll
void bro_tile_world_TileWorld_rebuildAll(void* self);

// bro.tile_world.TileWorld.prototype.save
//   registered at __bro_native.tile_world.TileWorld_save
//   result: u8[] in *out; out->release == NULL, the runtime copies
void bro_tile_world_TileWorld_save(void* self, bronze_native_buffer* out);

// bro.tile_world.TileWorld.prototype.load
//   registered at __bro_native.tile_world.TileWorld_load
bool bro_tile_world_TileWorld_load(void* self, const uint8_t* data, uint32_t data_len);

// bro.tile_world.TileWorld.prototype.destroy
//   registered at __bro_native.tile_world.TileWorld_destroy
void bro_tile_world_TileWorld_destroy(void* self);

// Registered in native_tile_world_register.cpp as well; the bodies are in the native_*.cpp files.
const char* bro_tile_world_TileWorld_distanceField(void* self, const char* sourcesJson, const char* jsonOpts);
const char* bro_tile_world_TileWorld_floodFill(void* self, int32_t seedX, int32_t seedY, const char* jsonOpts);
const char* bro_tile_world_TileWorld_components(void* self, const char* jsonOpts);
int32_t bro_tile_world_TileWorld_cellDistance(void* self, int32_t ax, int32_t ay, int32_t bx, int32_t by, const char* connStr);
const char* bro_tile_world_TileWorld_cellRing(void* self, int32_t cx, int32_t cy, int32_t radius, const char* connStr);
const char* bro_tile_world_TileWorld_cellsInRange(void* self, int32_t cx, int32_t cy, int32_t radius, const char* connStr);
const char* bro_tile_world_TileWorld_cellLine(void* self, int32_t ax, int32_t ay, int32_t bx, int32_t by);
const char* bro_tile_world_TileWorld_cellNeighbors(void* self, int32_t cx, int32_t cy, const char* connStr);
int32_t bro_tile_world_TileWorld_addObjectKind(void* self, uint64_t meshVal, const char* styleJson);
bool bro_tile_world_TileWorld_replaceObjectKind(void* self, int32_t kind, uint64_t meshVal, bool hasStyle, const char* styleJson);
bool bro_tile_world_TileWorld_removeObjectKind(void* self, int32_t kind);
bool bro_tile_world_TileWorld_hasObjectKind(void* self, int32_t kind);
int32_t bro_tile_world_TileWorld_addObjectPlacement(void* self, int32_t kind, int32_t x, int32_t y, double yaw, double scale, double yOffset, double offsetX, double offsetZ, int32_t variant, const double* color, uint32_t color_len);
bool bro_tile_world_TileWorld_replaceObjectPlacement(void* self, int32_t kind, int32_t index, int32_t x, int32_t y, double yaw, double scale, double yOffset, double offsetX, double offsetZ, int32_t variant, const double* color, uint32_t color_len);
bool bro_tile_world_TileWorld_removeObject(void* self, int32_t kind, int32_t index);
void* bro_scene_SceneGraph_createTileWorldJson(void* self, const char* configJson);
void bro_tile_world_TileWorld_findPathJson(void* self, int32_t startX, int32_t startY, int32_t endX, int32_t endY, const char* jsonOpts);
void bro_tile_world_TileWorld_fillTile(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t tileId, int32_t layer);
void bro_tile_world_TileWorld_setElevation(void* self, int32_t x, int32_t y, int32_t level);
int32_t bro_tile_world_TileWorld_getElevation(void* self, int32_t x, int32_t y);
void bro_tile_world_TileWorld_fillElevation(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t level);
void bro_tile_world_TileWorld_setFlag(void* self, int32_t x, int32_t y, double bit, bool on);
bool bro_tile_world_TileWorld_hasFlag(void* self, int32_t x, int32_t y, double bit);
void bro_tile_world_TileWorld_setTint(void* self, int32_t x, int32_t y, double r, double g, double b, double a);
void bro_tile_world_TileWorld_fillTint(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double r, double g, double b, double a);
const char* bro_tile_world_TileWorld_getTint(void* self, int32_t x, int32_t y);
const char* bro_tile_world_TileWorld_worldToCell(void* self, double wx, double wz);
const char* bro_tile_world_TileWorld_cellCenterWorldXZ(void* self, int32_t cx, int32_t cy);
const char* bro_tile_world_TileWorld_worldBounds(void* self);
double bro_tile_world_TileWorld_sampleHeight(void* self, double wx, double wz);
const char* bro_tile_world_TileWorld_raycastCell(void* self, double ox, double oy, double oz, double dx, double dy, double dz, double maxDist);
bool bro_tile_world_TileWorld_isWalkable(void* self, int32_t x, int32_t y, double mask);
void bro_tile_world_TileWorld_configure(void* self, const char* configJson);
void bro_tile_world_TileWorld_setShade(void* self, int32_t x, int32_t y, double v);
void bro_tile_world_TileWorld_fillShade(void* self, int32_t x0, int32_t y0, int32_t x1, int32_t y1, double v);
void bro_tile_world_TileWorld_setShadeMapFloat(void* self, const float* data, uint32_t count);
void bro_tile_world_TileWorld_setShadeMapBytes(void* self, const uint8_t* data, uint32_t count);
double bro_tile_world_TileWorld_getShade(void* self, int32_t x, int32_t y);
void* bro_scene_SceneGraph_createTileWorld(void* self, bool opts_chunkSize_given, int32_t opts_chunkSize,
                                          bool opts_tileSize_given, double opts_tileSize,
                                          const char* opts_layers, bool opts_tileAtlas_given, const char* opts_tileAtlas,
                                          bool opts_atlasTileWidth_given, int32_t opts_atlasTileWidth,
                                          bool opts_atlasTileHeight_given, int32_t opts_atlasTileHeight);

#ifdef __cplusplus
}  // extern "C"
#endif
