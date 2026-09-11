// =============================================================================
// bro_domparser_c_abi.h — Pure C-ABI declarations for bro.domparser
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_DOMPARSER_C_ABI_H
#define BRO_DOMPARSER_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.domparser.DOMParser ---
void* bro_DOMParser_create(void);
void  bro_DOMParser_destroy(void* self);
void* bro_DOMParser_parseFromString(void* self, const char* str, const char* type);

#ifdef __cplusplus
}
#endif

#endif // BRO_DOMPARSER_C_ABI_H
