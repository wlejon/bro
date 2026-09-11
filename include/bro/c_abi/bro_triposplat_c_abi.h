// =============================================================================
// bro_triposplat_c_abi.h — Pure C-ABI declarations for bro.triposplat
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_TRIPOSPLAT_C_ABI_H
#define BRO_TRIPOSPLAT_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.triposplat.TripoSplatPipeline ---
void* bro_TripoSplatPipeline_create(void);
void  bro_TripoSplatPipeline_destroy(void* self);

// --- Namespace bro.triposplat ---
void bro_triposplat_init(void);
void* bro_triposplat_load(void* config);
void bro_triposplat_cancel(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_TRIPOSPLAT_C_ABI_H
