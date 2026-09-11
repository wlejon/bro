// =============================================================================
// bro_animation_c_abi.h — Pure C-ABI declarations for bro.animation
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_ANIMATION_C_ABI_H
#define BRO_ANIMATION_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.animation.Tween ---
void* bro_Tween_create(void);
void  bro_Tween_destroy(void* self);
void* bro_Tween_to(void* self, void* target, void* props, double duration, const char* easing);
void* bro_Tween_parallel(void* self, void* tweens);
void* bro_Tween_call(void* self, void* callback);
void* bro_Tween_loop(void* self, int32_t count);
void* bro_Tween_start(void* self);
void* bro_Tween_stop(void* self);
void* bro_Tween_pause(void* self);
void* bro_Tween_resume(void* self);
void bro_Tween_destroy(void* self);

// --- Interface bro.animation.AnimationPlayer ---
void* bro_AnimationPlayer_create(void);
void  bro_AnimationPlayer_destroy(void* self);
void bro_AnimationPlayer_addClip(void* self, const char* name, void* clip);
void bro_AnimationPlayer_clipDef(void* self, const char* name, void* def);
void bro_AnimationPlayer_play(void* self, const char* clipName, void* opts);
void bro_AnimationPlayer_pause(void* self);
void bro_AnimationPlayer_resume(void* self);
void bro_AnimationPlayer_stop(void* self);
void bro_AnimationPlayer_seek(void* self, double time);
void bro_AnimationPlayer_destroy(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_ANIMATION_C_ABI_H
