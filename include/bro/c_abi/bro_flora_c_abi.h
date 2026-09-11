// =============================================================================
// bro_flora_c_abi.h — Pure C-ABI declarations for bro.flora
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_FLORA_C_ABI_H
#define BRO_FLORA_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.flora ---
void* bro_flora_createWorld(void* opts);
void* bro_flora_leafCluster(void* phyllotaxy, void* opts);
void bro_flora_setWind(double strength, double dirX, double dirY);
void bro_flora_wind(double strength, double dirX, double dirY);
void bro_flora_setDensity(double density);
void bro_flora_density(double density);
void bro_flora_update(double dt);
void bro_flora_clear(void);
void bro_flora_placement(void* config);
void bro_flora_addPlacement(void* config);
void* bro_flora_batches(void);
void* bro_flora_getBatches(void);

// --- Interface bro.flora.FloraWorld ---
void* bro_FloraWorld_create(void);
void  bro_FloraWorld_destroy(void* self);
int32_t bro_FloraWorld_addPrototype(void* self, void* spec);
void* bro_FloraWorld_addVoronoiSite(void* self, int32_t prototypeIndex, double determinacy, double apicalControl);
int32_t bro_FloraWorld_addPlant(void* self, void* spec);
bool bro_FloraWorld_removePlant(void* self, int32_t plantIdx);
void* bro_FloraWorld_step(void* self, double dt);
void* bro_FloraWorld_plantInfo(void* self, int32_t plantIdx);
void* bro_FloraWorld_setClimate(void* self, void* opts);
double bro_FloraWorld_sampleShadow(void* self, void* pos);
const char* bro_FloraWorld_validate(void* self);
void* bro_FloraWorld_emitMesh(void* self, int32_t sides);
void* bro_FloraWorld_emitSegments(void* self);
void* bro_FloraWorld_emitFoliage(void* self);
void* bro_FloraWorld_emitBloomAnchors(void* self);
double bro_FloraWorld_get_simTime(void* self);
int32_t bro_FloraWorld_get_plantCount(void* self);
int32_t bro_FloraWorld_get_prototypeCount(void* self);
int32_t bro_FloraWorld_get_moduleCount(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_FLORA_C_ABI_H
