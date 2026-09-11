// =============================================================================
// bro_events_c_abi.h — Pure C-ABI declarations for bro.events
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_EVENTS_C_ABI_H
#define BRO_EVENTS_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.events.Event ---
void* bro_Event_create(const char* type, void* eventInitDict);
void  bro_Event_destroy(void* self);
const char* bro_Event_get_type(void* self);
void* bro_Event_get_target(void* self);
void* bro_Event_get_currentTarget(void* self);
bool bro_Event_get_bubbles(void* self);
bool bro_Event_get_cancelable(void* self);
bool bro_Event_get_defaultPrevented(void* self);
double bro_Event_get_timeStamp(void* self);
void bro_Event_preventDefault(void* self);
void bro_Event_stopPropagation(void* self);
void bro_Event_initEvent(void* self, const char* type, bool bubbles, bool cancelable);

// --- Interface bro.events.CustomEvent ---
void* bro_CustomEvent_create(void);
void  bro_CustomEvent_destroy(void* self);
void* bro_CustomEvent_get_detail(void* self);

// --- Interface bro.events.UIEvent ---
void* bro_UIEvent_create(void);
void  bro_UIEvent_destroy(void* self);
void* bro_UIEvent_get_view(void* self);
int32_t bro_UIEvent_get_detail(void* self);

// --- Interface bro.events.MouseEvent ---
void* bro_MouseEvent_create(void);
void  bro_MouseEvent_destroy(void* self);
double bro_MouseEvent_get_screenX(void* self);
double bro_MouseEvent_get_screenY(void* self);
double bro_MouseEvent_get_clientX(void* self);
double bro_MouseEvent_get_clientY(void* self);
double bro_MouseEvent_get_offsetX(void* self);
double bro_MouseEvent_get_offsetY(void* self);
double bro_MouseEvent_get_pageX(void* self);
double bro_MouseEvent_get_pageY(void* self);
bool bro_MouseEvent_get_ctrlKey(void* self);
bool bro_MouseEvent_get_shiftKey(void* self);
bool bro_MouseEvent_get_altKey(void* self);
bool bro_MouseEvent_get_metaKey(void* self);
int16_t bro_MouseEvent_get_button(void* self);
uint16_t bro_MouseEvent_get_buttons(void* self);
void* bro_MouseEvent_get_relatedTarget(void* self);

// --- Interface bro.events.PointerEvent ---
void* bro_PointerEvent_create(void);
void  bro_PointerEvent_destroy(void* self);
int32_t bro_PointerEvent_get_pointerId(void* self);
const char* bro_PointerEvent_get_pointerType(void* self);
bool bro_PointerEvent_get_isPrimary(void* self);
double bro_PointerEvent_get_pressure(void* self);
double bro_PointerEvent_get_width(void* self);
double bro_PointerEvent_get_height(void* self);

// --- Interface bro.events.Touch ---
void* bro_Touch_create(void);
void  bro_Touch_destroy(void* self);
int32_t bro_Touch_get_identifier(void* self);
void* bro_Touch_get_target(void* self);
double bro_Touch_get_screenX(void* self);
double bro_Touch_get_screenY(void* self);
double bro_Touch_get_clientX(void* self);
double bro_Touch_get_clientY(void* self);
double bro_Touch_get_pageX(void* self);
double bro_Touch_get_pageY(void* self);
double bro_Touch_get_force(void* self);

// --- Interface bro.events.TouchList ---
void* bro_TouchList_create(void);
void  bro_TouchList_destroy(void* self);
uint32_t bro_TouchList_get_length(void* self);
void* bro_TouchList_item(void* self, uint32_t index);

// --- Interface bro.events.TouchEvent ---
void* bro_TouchEvent_create(void);
void  bro_TouchEvent_destroy(void* self);
void* bro_TouchEvent_get_touches(void* self);
void* bro_TouchEvent_get_targetTouches(void* self);
void* bro_TouchEvent_get_changedTouches(void* self);
bool bro_TouchEvent_get_ctrlKey(void* self);
bool bro_TouchEvent_get_shiftKey(void* self);
bool bro_TouchEvent_get_altKey(void* self);
bool bro_TouchEvent_get_metaKey(void* self);

// --- Interface bro.events.GestureEvent ---
void* bro_GestureEvent_create(void);
void  bro_GestureEvent_destroy(void* self);
double bro_GestureEvent_get_scale(void* self);
double bro_GestureEvent_get_rotation(void* self);
double bro_GestureEvent_get_clientX(void* self);
double bro_GestureEvent_get_clientY(void* self);

#ifdef __cplusplus
}
#endif

#endif // BRO_EVENTS_C_ABI_H
