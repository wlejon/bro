// =============================================================================
// bro_rave_c_abi.h — Pure C-ABI declarations for bro.rave
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_RAVE_C_ABI_H
#define BRO_RAVE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.rave.Rave ---
void* bro_Rave_create(void);
void  bro_Rave_destroy(void* self);
bool bro_Rave_get_loaded(void* self);
int32_t bro_Rave_get_sampleRate(void* self);
int32_t bro_Rave_get_nLatent(void* self);
int32_t bro_Rave_get_fullLatent(void* self);
int32_t bro_Rave_get_nBand(void* self);
int32_t bro_Rave_get_totalRatio(void* self);
void* bro_Rave_encode(void* self, void* audio);
void* bro_Rave_decode(void* self, void* latent, int32_t frames, void* opts);

// --- Namespace bro.rave ---
void bro_rave_init(void);
void* bro_rave_loadRave(const char* modelDir, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_RAVE_C_ABI_H
