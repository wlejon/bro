// =============================================================================
// bro_server_c_abi.h — Pure C-ABI declarations for bro.server
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_SERVER_C_ABI_H
#define BRO_SERVER_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.server ---
double bro_server_get_tickrate(void);
void bro_server_set_tickrate(double val);
double bro_server_get_uptime(void);
void bro_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_SERVER_C_ABI_H
