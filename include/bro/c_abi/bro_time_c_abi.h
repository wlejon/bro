// =============================================================================
// bro_time_c_abi.h — Pure C-ABI declarations for bro.time
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_TIME_C_ABI_H
#define BRO_TIME_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.time ---
double bro_time_get_scale(void);
void bro_time_set_scale(double val);
bool bro_time_get_paused(void);
void bro_time_set_paused(bool val);
double bro_time_get_now(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_TIME_C_ABI_H
