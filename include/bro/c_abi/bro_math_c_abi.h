// =============================================================================
// bro_math_c_abi.h — Pure C-ABI declarations for bro.math
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_MATH_C_ABI_H
#define BRO_MATH_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.math ---
double bro_math_lerp(double a, double b, double t);
double bro_math_clamp(double x, double lo, double hi);
double bro_math_saturate(double x);
double bro_math_invLerp(double a, double b, double x);
double bro_math_remap(double x, double inMin, double inMax, double outMin, double outMax);
double bro_math_smoothstep(double e0, double e1, double x);
double bro_math_smootherstep(double e0, double e1, double x);
double bro_math_degToRad(double deg);
double bro_math_radToDeg(double rad);
double bro_math_wrapAngle(double a);
double bro_math_angleDiff(double a, double b);
double bro_math_fnv1a32(const char* data, double seed);
double bro_math_hashU32(double x);
double bro_math_cellHash(double x, double y, double z);

// --- Interface bro.math.SpatialHash3D ---
void* bro_SpatialHash3D_create(double cellSize, double bucketCount);
void  bro_SpatialHash3D_destroy(void* self);
void* bro_SpatialHash3D_insert(void* self, double id, double x, double y, double z);
bool bro_SpatialHash3D_remove(void* self, double id, double x, double y, double z);
void* bro_SpatialHash3D_queryRadius(void* self, double x, double y, double z, double radius);
void* bro_SpatialHash3D_queryAABB(void* self, double minX, double minY, double minZ, double maxX, double maxY, double maxZ);
double bro_SpatialHash3D_nearest(void* self, double x, double y, double z, double maxDist);
void* bro_SpatialHash3D_clear(void* self);
double bro_SpatialHash3D_get_size(void* self);

// --- Interface bro.math.Rng ---
void* bro_Rng_create(double seed);
void  bro_Rng_destroy(void* self);
void* bro_Rng_reseed(void* self, double seed);
double bro_Rng_float01(void* self);
double bro_Rng_signed(void* self);
double bro_Rng_range(void* self, double lo, double hi);
int32_t bro_Rng_int(void* self, int32_t lo, int32_t hi);
double bro_Rng_uint32(void* self);
double bro_Rng_normal(void* self);
void* bro_Rng_gaussian2D(void* self, double sigma);
void* bro_Rng_inUnitDisc(void* self);
void* bro_Rng_inUnitSphere(void* self);
void* bro_Rng_onUnitSphere(void* self);

// --- Interface bro.math.Smoother ---
void* bro_Smoother_create(double timeMs, double sampleRate);
void  bro_Smoother_destroy(void* self);
void* bro_Smoother_setTime(void* self, double timeMs, double sampleRate);
void* bro_Smoother_reset(void* self, double value);
void* bro_Smoother_setTarget(void* self, double t);
double bro_Smoother_tick(void* self);
double bro_Smoother_tickN(void* self, int32_t n);
double bro_Smoother_get_current(void* self);
double bro_Smoother_get_target(void* self);
double bro_Smoother_get_coeff(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_MATH_C_ABI_H
