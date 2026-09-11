// =============================================================================
// bro_vendor_globals_c_abi.h — Pure C-ABI declarations for bro.vendor_globals
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_VENDOR_GLOBALS_C_ABI_H
#define BRO_VENDOR_GLOBALS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Namespace bro.vendor_globals ---
void* bro_vendor_globals_get_signals(void);
void* bro_vendor_globals_get_CodeMirror(void);
void* bro_vendor_globals_get_acorn(void);
void* bro_vendor_globals_get_tern(void);
void* bro_vendor_globals_get_esprima(void);
void* bro_vendor_globals_get_jsonlint(void);
void* bro_vendor_globals_get_draco_encoder(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_VENDOR_GLOBALS_C_ABI_H
