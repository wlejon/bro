// =============================================================================
// bro_wake_c_abi.h — Pure C-ABI declarations for bro.wake
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_WAKE_C_ABI_H
#define BRO_WAKE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.wake.WakeStreamView ---
void* bro_WakeStreamView_create(void);
void  bro_WakeStreamView_destroy(void* self);
bool bro_WakeStreamView_get_active(void* self);
void bro_WakeStreamView_listen(void* self, void* opts);
void bro_WakeStreamView_stop(void* self);
void bro_WakeStreamView_suspend(void* self);
void bro_WakeStreamView_resume(void* self);
double bro_WakeStreamView_lastScore(void* self);
bool bro_WakeStreamView_isActive(void* self);
bool bro_WakeStreamView_isSuspended(void* self);
bool bro_WakeStreamView_isLoaded(void* self);
void bro_WakeStreamView_setThreshold(void* self, double threshold);
void* bro_WakeStreamView_stats(void* self);
void* bro_WakeStreamView_feed(void* self, void* samples, int32_t sampleRate);

// --- Namespace bro.wake ---
void bro_wake_init(void);
void bro_wake_load(void* opts);
void bro_wake_unload(void);
void bro_wake_listen(void* opts);
void bro_wake_stop(void);
void bro_wake_suspend(void);
void bro_wake_resume(void);
double bro_wake_lastScore(void);
bool bro_wake_isActive(void);
bool bro_wake_isSuspended(void);
bool bro_wake_isLoaded(void);
void bro_wake_setThreshold(double threshold);
void* bro_wake_stats(void);
void* bro_wake_feed(void* samples, int32_t sampleRate);

#ifdef __cplusplus
}
#endif

#endif // BRO_WAKE_C_ABI_H
