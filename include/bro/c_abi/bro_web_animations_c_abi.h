// =============================================================================
// bro_web_animations_c_abi.h — Pure C-ABI declarations for bro.web_animations
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_WEB_ANIMATIONS_C_ABI_H
#define BRO_WEB_ANIMATIONS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.web_animations.Animation ---
void* bro_Animation_create(void);
void  bro_Animation_destroy(void* self);
double bro_Animation_get_currentTime(void* self);
void bro_Animation_set_currentTime(void* self, double val);
double bro_Animation_get_playbackRate(void* self);
void bro_Animation_set_playbackRate(void* self, double val);
const char* bro_Animation_get_playState(void* self);
bool bro_Animation_get_pending(void* self);
void* bro_Animation_get_finished(void* self);
void* bro_Animation_get_ready(void* self);
void* bro_Animation_get_onfinish(void* self);
void bro_Animation_set_onfinish(void* self, void* val);
void* bro_Animation_get_oncancel(void* self);
void bro_Animation_set_oncancel(void* self, void* val);
void bro_Animation_play(void* self);
void bro_Animation_pause(void* self);
void bro_Animation_finish(void* self);
void bro_Animation_cancel(void* self);
void bro_Animation_reverse(void* self);

// --- Interface bro.web_animations.WebAnimations ---
void* bro_WebAnimations_create(void);
void  bro_WebAnimations_destroy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_WEB_ANIMATIONS_C_ABI_H
