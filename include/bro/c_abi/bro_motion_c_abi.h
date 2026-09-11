// =============================================================================
// bro_motion_c_abi.h — Pure C-ABI declarations for bro.motion
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_MOTION_C_ABI_H
#define BRO_MOTION_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.motion.ArdyMotionPipeline ---
void* bro_ArdyMotionPipeline_create(void);
void  bro_ArdyMotionPipeline_destroy(void* self);
const char* bro_ArdyMotionPipeline_get_device(void* self);
void* bro_ArdyMotionPipeline_generate(void* self, const char* text, void* opts);

// --- Namespace bro.motion ---
void bro_motion_init(void);
void* bro_motion_load(void* opts);

#ifdef __cplusplus
}
#endif

#endif // BRO_MOTION_C_ABI_H
