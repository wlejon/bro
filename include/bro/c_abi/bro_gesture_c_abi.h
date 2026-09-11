// =============================================================================
// bro_gesture_c_abi.h — Pure C-ABI declarations for bro.gesture
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_GESTURE_C_ABI_H
#define BRO_GESTURE_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.gesture.GestureStreamView ---
void* bro_GestureStreamView_create(void);
void  bro_GestureStreamView_destroy(void* self);
bool bro_GestureStreamView_get_active(void* self);
int32_t bro_GestureStreamView_enrollFromAudio(void* self, const char* name, void* samples, void* policy);
bool bro_GestureStreamView_remove(void* self, const char* name);
void bro_GestureStreamView_clear(void* self);
void* bro_GestureStreamView_templates(void* self);
void* bro_GestureStreamView_inspect(void* self, const char* name);
void bro_GestureStreamView_reset(void* self);
void bro_GestureStreamView_listen(void* self, void* opts);
void bro_GestureStreamView_stop(void* self);
bool bro_GestureStreamView_isActive(void* self);
int32_t bro_GestureStreamView_sampleRate(void* self);

// --- Namespace bro.gesture ---
void bro_gesture_init(void);
int32_t bro_gesture_enrollFromAudio(const char* name, void* samples, void* policy);
bool bro_gesture_remove(const char* name);
void bro_gesture_clear(void);
void* bro_gesture_templates(void);
void* bro_gesture_inspect(const char* name);
void bro_gesture_reset(void);
void bro_gesture_listen(void* opts);
void bro_gesture_stop(void);
bool bro_gesture_isActive(void);
int32_t bro_gesture_sampleRate(void);

#ifdef __cplusplus
}
#endif

#endif // BRO_GESTURE_C_ABI_H
