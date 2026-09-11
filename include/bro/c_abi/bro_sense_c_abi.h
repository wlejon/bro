// =============================================================================
// bro_sense_c_abi.h — Pure C-ABI declarations for bro.sense
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_SENSE_C_ABI_H
#define BRO_SENSE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.sense.SenseStreamView ---
void* bro_SenseStreamView_create(void);
void  bro_SenseStreamView_destroy(void* self);
bool bro_SenseStreamView_get_active(void* self);
void bro_SenseStreamView_start(void* self, void* opts);
void bro_SenseStreamView_stop(void* self);
bool bro_SenseStreamView_isActive(void* self);
void* bro_SenseStreamView_snapshot(void* self);
int32_t bro_SenseStreamView_sampleRate(void* self);
void* bro_SenseStreamView_stats(void* self);
void* bro_SenseStreamView_feed(void* self, void* samples);
void* bro_SenseStreamView_analyze(void* self, void* samples, void* opts);

// --- Namespace bro.sense ---
void bro_sense_init(void);
void bro_sense_start(void* opts);
void bro_sense_stop(void);
bool bro_sense_isActive(void);
void* bro_sense_snapshot(void);
int32_t bro_sense_sampleRate(void);
void* bro_sense_stats(void);
void* bro_sense_feed(void* samples);
void* bro_sense_analyze(void* samples, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_SENSE_C_ABI_H
