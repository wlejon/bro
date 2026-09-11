// =============================================================================
// bro_diffusion_c_abi.h — Pure C-ABI declarations for bro.diffusion
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_DIFFUSION_C_ABI_H
#define BRO_DIFFUSION_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.diffusion.Pipeline ---
void* bro_Pipeline_create(void);
void  bro_Pipeline_destroy(void* self);

// --- Interface bro.diffusion.PipelineState ---
void* bro_PipelineState_create(void);
void  bro_PipelineState_destroy(void* self);

// --- Namespace bro.diffusion ---
const char* bro_diffusion_get_version(void);
void bro_diffusion_init(void);
void* bro_diffusion_createPipeline(void* config);
void* bro_diffusion_loadModel(const char* dir, void* opts);
void* bro_diffusion_expandNoise(void* src, void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_DIFFUSION_C_ABI_H
