// =============================================================================
// bro_noise_c_abi.h — Pure C-ABI declarations for bro.noise
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_NOISE_C_ABI_H
#define BRO_NOISE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.noise.FastNoise ---
void* bro_FastNoise_create_from_encoded(const char* encodedNodeTree);
void  bro_FastNoise_destroy(void* self);
void* bro_FastNoise_create(const char* typeName);
void* bro_FastNoise_types(void);
void* bro_FastNoise_Simplex(void);
void* bro_FastNoise_SuperSimplex(void);
void* bro_FastNoise_Perlin(void);
void* bro_FastNoise_Value(void);
void* bro_FastNoise_CellularValue(void);
void* bro_FastNoise_CellularDistance(void);
void* bro_FastNoise_CellularLookup(void);
void* bro_FastNoise_FractalFBm(void);
void* bro_FastNoise_FractalRidged(void);
void* bro_FastNoise_DomainWarpGradient(void);
void bro_FastNoise_set(void* self, const char* name, double val);
void* bro_FastNoise_getMembers(void* self);
double bro_FastNoise_genSingle2D(void* self, double x, double y, int32_t seed);
double bro_FastNoise_genSingle3D(void* self, double x, double y, double z, int32_t seed);
void* bro_FastNoise_genUniformGrid2D(void* self, double xOffset, double yOffset, int32_t xSize, int32_t ySize, double frequency, int32_t seed);
void bro_FastNoise_genUniformGrid2DInto(void* self, void* dest, double xOffset, double yOffset, int32_t xSize, int32_t ySize, double frequency, int32_t seed);
void* bro_FastNoise_genUniformGrid3D(void* self, double xOff, double yOff, double zOff, int32_t xSize, int32_t ySize, int32_t zSize, double frequency, int32_t seed);
void bro_FastNoise_genUniformGrid3DInto(void* self, void* dest, double xOff, double yOff, double zOff, int32_t xSize, int32_t ySize, int32_t zSize, double frequency, int32_t seed);
void bro_FastNoise_genPositionArray2D(void* self, void* dest, void* xs, void* ys, double x_off, double y_off, int32_t seed);
void bro_FastNoise_genPositionArray3D(void* self, void* dest, void* xs, void* ys, void* zs, double x_off, double y_off, double z_off, int32_t seed);
void* bro_FastNoise_genTileable2D(void* self, int32_t xSize, int32_t ySize, double frequency, int32_t seed);
void bro_FastNoise_set_node(void* self, const char* name, void* node);

#ifdef __cplusplus
}
#endif

#endif // BRO_NOISE_C_ABI_H
