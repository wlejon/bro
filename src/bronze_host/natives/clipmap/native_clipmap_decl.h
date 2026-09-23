// native_clipmap_decl.h — the C entry points native_clipmap_register.cpp registers, one
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
// Gate: BRO_WITH_3D (native_clipmap_register.cpp registers nothing when it is off).
//
// Names classes of: scene — register those subsystems first.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "abi/bronze_native_type.h"  // bronze_native_buffer, owned by bronze

#ifdef __cplusplus
extern "C" {
#endif

// destructor of __bro_native.clipmap.ClipmapTerrain: frees what the constructor (or a native returning a ClipmapTerrain) allocated
void bro_clipmap_ClipmapTerrain_dtor(void* self);

// constructor of __bro_native.clipmap.ClipmapTerrain: registered so the class exists (instances come from natives returning a ClipmapTerrain); never called by the wrapper, the body may return NULL
//   registered at __bro_native.clipmap.ClipmapTerrain
void* bro_clipmap_ClipmapTerrain_ctor(void);

// bro.clipmap.ClipmapTerrain.prototype.node read
//   registered at __bro_native.clipmap.ClipmapTerrain_node_get
void* bro_clipmap_ClipmapTerrain_node_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.levels read
//   registered at __bro_native.clipmap.ClipmapTerrain_levels_get
int32_t bro_clipmap_ClipmapTerrain_levels_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.resolution read
//   registered at __bro_native.clipmap.ClipmapTerrain_resolution_get
int32_t bro_clipmap_ClipmapTerrain_resolution_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.cellSize read
//   registered at __bro_native.clipmap.ClipmapTerrain_cellSize_get
double bro_clipmap_ClipmapTerrain_cellSize_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.layerCount read
//   registered at __bro_native.clipmap.ClipmapTerrain_layerCount_get
int32_t bro_clipmap_ClipmapTerrain_layerCount_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.triangleCount read
//   registered at __bro_native.clipmap.ClipmapTerrain_triangleCount_get
int32_t bro_clipmap_ClipmapTerrain_triangleCount_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.vertexCount read
//   registered at __bro_native.clipmap.ClipmapTerrain_vertexCount_get
int32_t bro_clipmap_ClipmapTerrain_vertexCount_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.farDistance read
//   registered at __bro_native.clipmap.ClipmapTerrain_farDistance_get
double bro_clipmap_ClipmapTerrain_farDistance_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.cellScale read
//   registered at __bro_native.clipmap.ClipmapTerrain_cellScale_get
double bro_clipmap_ClipmapTerrain_cellScale_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.planetRadius read
//   registered at __bro_native.clipmap.ClipmapTerrain_planetRadius_get
double bro_clipmap_ClipmapTerrain_planetRadius_get(void* self);

// bro.clipmap.ClipmapTerrain.prototype.setSnowLine
//   registered at __bro_native.clipmap.ClipmapTerrain_setSnowLine
void* bro_clipmap_ClipmapTerrain_setSnowLine(void* self, double m);

// bro.clipmap.ClipmapTerrain.prototype.setDetail
//   registered at __bro_native.clipmap.ClipmapTerrain_setDetail
//   desc_wavelength_given, desc_relief_given, desc_gain_given, desc_octaves_given: false when the value was not passed (no declared default)
void* bro_clipmap_ClipmapTerrain_setDetail(void* self, bool desc_wavelength_given, double desc_wavelength, bool desc_relief_given, double desc_relief, bool desc_gain_given, double desc_gain, bool desc_octaves_given, int32_t desc_octaves);

// bro.clipmap.ClipmapTerrain.prototype.setMaterials
//   registered at __bro_native.clipmap.ClipmapTerrain_setMaterials
//   desc_rock_roughness_given, desc_snow_roughness_given, desc_sand_roughness_given, desc_grass_roughness_given: false when the value was not passed (no declared default)
void* bro_clipmap_ClipmapTerrain_setMaterials(void* self, const double* desc_rock_albedo, uint32_t desc_rock_albedo_len, bool desc_rock_roughness_given, double desc_rock_roughness, const double* desc_snow_albedo, uint32_t desc_snow_albedo_len, bool desc_snow_roughness_given, double desc_snow_roughness, const double* desc_sand_albedo, uint32_t desc_sand_albedo_len, bool desc_sand_roughness_given, double desc_sand_roughness, const double* desc_grass_albedo, uint32_t desc_grass_albedo_len, bool desc_grass_roughness_given, double desc_grass_roughness);

// bro.clipmap.ClipmapTerrain.prototype.setForest
//   registered at __bro_native.clipmap.ClipmapTerrain_setForest
//   desc_strength_given: false when the value was not passed (no declared default)
void* bro_clipmap_ClipmapTerrain_setForest(void* self, const double* desc_albedo, uint32_t desc_albedo_len, bool desc_strength_given, double desc_strength);

// bro.clipmap.ClipmapTerrain.prototype.update
//   registered at __bro_native.clipmap.ClipmapTerrain_update
void* bro_clipmap_ClipmapTerrain_update(void* self, double camX, double camY, double camZ);

// bro.clipmap.ClipmapTerrain.prototype.shaderSource
//   registered at __bro_native.clipmap.ClipmapTerrain_shaderSource
const char* bro_clipmap_ClipmapTerrain_shaderSource(void* self, const char* stage);

// bro.clipmap.ClipmapTerrain.prototype.elevationAt
//   registered at __bro_native.clipmap.ClipmapTerrain_elevationAt
double bro_clipmap_ClipmapTerrain_elevationAt(void* self, double x, double z);

// bro.clipmap.ClipmapTerrain.prototype.renderedElevationAt
//   registered at __bro_native.clipmap.ClipmapTerrain_renderedElevationAt
double bro_clipmap_ClipmapTerrain_renderedElevationAt(void* self, double x, double z);

// bro.clipmap.ClipmapTerrain.prototype.coverageDistance
//   registered at __bro_native.clipmap.ClipmapTerrain_coverageDistance
double bro_clipmap_ClipmapTerrain_coverageDistance(void* self, double eyeAboveSeaLevel);

// bro.clipmap.ClipmapTerrain.prototype.horizonDistance
//   registered at __bro_native.clipmap.ClipmapTerrain_horizonDistance
double bro_clipmap_ClipmapTerrain_horizonDistance(void* self, double eyeAboveSeaLevel);

// bro.clipmap.ClipmapTerrain.prototype.destroy
//   registered at __bro_native.clipmap.ClipmapTerrain_destroy
void bro_clipmap_ClipmapTerrain_destroy(void* self);

#ifdef __cplusplus
}  // extern "C"
#endif
