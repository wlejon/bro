// =============================================================================
// bro_gamepad_c_abi.h — Pure C-ABI declarations for bro.gamepad
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, zero JSContext, direct native CPU register call.
// =============================================================================

#ifndef BRO_GAMEPAD_C_ABI_H
#define BRO_GAMEPAD_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.gamepad.GamepadHapticActuator ---
void* bro_GamepadHapticActuator_create(void);
void  bro_GamepadHapticActuator_destroy(void* self);
const char* bro_GamepadHapticActuator_get_type(void* self);
void* bro_GamepadHapticActuator_get_effects(void* self);
void* bro_GamepadHapticActuator_playEffect(void* self, const char* type, void* params);
void* bro_GamepadHapticActuator_reset(void* self);

// --- Interface bro.gamepad.GamepadButton ---
void* bro_GamepadButton_create(void);
void  bro_GamepadButton_destroy(void* self);
bool bro_GamepadButton_get_pressed(void* self);
bool bro_GamepadButton_get_touched(void* self);
double bro_GamepadButton_get_value(void* self);

// --- Interface bro.gamepad.Gamepad ---
void* bro_Gamepad_create(void);
void  bro_Gamepad_destroy(void* self);
const char* bro_Gamepad_get_id(void* self);
int32_t bro_Gamepad_get_index(void* self);
bool bro_Gamepad_get_connected(void* self);
const char* bro_Gamepad_get_mapping(void* self);
void* bro_Gamepad_get_buttons(void* self);
void* bro_Gamepad_get_axes(void* self);
double bro_Gamepad_get_timestamp(void* self);
void* bro_Gamepad_get_vibrationActuator(void* self);

// --- Interface bro.gamepad.GamepadEvent ---
void* bro_GamepadEvent_create(void);
void  bro_GamepadEvent_destroy(void* self);
void* bro_GamepadEvent_get_gamepad(void* self);

// --- Namespace bro.gamepad ---
bool bro_gamepad_isConnected(int32_t index);
double bro_gamepad_getAxis(int32_t index, int32_t axis);
double bro_gamepad_getButton(int32_t index, int32_t button);
bool bro_gamepad_rumble(int32_t index, float strong, float weak, int32_t duration);
bool bro_gamepad_rumbleTriggers(int32_t index, float left, float right, int32_t duration);

#ifdef __cplusplus
}
#endif

#endif // BRO_GAMEPAD_C_ABI_H
