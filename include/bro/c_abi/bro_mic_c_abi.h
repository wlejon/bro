// =============================================================================
// bro_mic_c_abi.h — Pure C-ABI declarations for bro.mic
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_MIC_C_ABI_H
#define BRO_MIC_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.mic ---
void bro_mic_start(void* opts);
void bro_mic_stop(void);
bool bro_mic_isActive(void);
int32_t bro_mic_engineRate(void);
void* bro_mic_stats(void);
void* bro_mic_levels(int32_t maxCount);
void bro_mic_feed(void* samples, int32_t sampleRate);

#ifdef __cplusplus
}
#endif

#endif // BRO_MIC_C_ABI_H
