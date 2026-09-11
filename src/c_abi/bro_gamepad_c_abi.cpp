// =============================================================================
// bro_gamepad_c_abi.cpp — C++ forwarding implementations for bro.gamepad
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_gamepad_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"

extern "C" {

// Namespace gamepad
bool bro_gamepad_isConnected(int32_t index) {
    const auto* b = bro_get_gamepad_bridge();
    if (b && b->isConnected) return b->isConnected(index);
    return false;
}

double bro_gamepad_getAxis(int32_t index, int32_t axis) {
    const auto* b = bro_get_gamepad_bridge();
    if (b && b->getAxis) return b->getAxis(index, axis);
    return 0.0;
}

double bro_gamepad_getButton(int32_t index, int32_t button) {
    const auto* b = bro_get_gamepad_bridge();
    if (b && b->getButton) return b->getButton(index, button);
    return 0.0;
}

bool bro_gamepad_rumble(int32_t index, float strong, float weak, int32_t duration) {
    const auto* b = bro_get_gamepad_bridge();
    if (b && b->rumble) return b->rumble(index, strong, weak, duration);
    return true;
}

bool bro_gamepad_rumbleTriggers(int32_t index, float left, float right, int32_t duration) {
    const auto* b = bro_get_gamepad_bridge();
    if (b && b->rumbleTriggers) return b->rumbleTriggers(index, left, right, duration);
    return true;
}

// GamepadHapticActuator
void* bro_GamepadHapticActuator_create(void) { return nullptr; }
void bro_GamepadHapticActuator_destroy(void* /*self*/) {}
const char* bro_GamepadHapticActuator_get_type(void* /*self*/) { return "dual-rumble"; }
void* bro_GamepadHapticActuator_get_effects(void* /*self*/) { return nullptr; }
void* bro_GamepadHapticActuator_playEffect(void* /*self*/, const char* /*type*/, void* /*params*/) { return nullptr; }
void* bro_GamepadHapticActuator_reset(void* /*self*/) { return nullptr; }

// GamepadButton
void* bro_GamepadButton_create(void) { return nullptr; }
void bro_GamepadButton_destroy(void* /*self*/) {}
bool bro_GamepadButton_get_pressed(void* /*self*/) { return false; }
bool bro_GamepadButton_get_touched(void* /*self*/) { return false; }
double bro_GamepadButton_get_value(void* /*self*/) { return 0.0; }

// Gamepad
void* bro_Gamepad_create(void) { return nullptr; }
void bro_Gamepad_destroy(void* /*self*/) {}
const char* bro_Gamepad_get_id(void* /*self*/) { return ""; }
int32_t bro_Gamepad_get_index(void* /*self*/) { return 0; }
bool bro_Gamepad_get_connected(void* /*self*/) { return false; }
const char* bro_Gamepad_get_mapping(void* /*self*/) { return "standard"; }
void* bro_Gamepad_get_buttons(void* /*self*/) { return nullptr; }
void* bro_Gamepad_get_axes(void* /*self*/) { return nullptr; }
double bro_Gamepad_get_timestamp(void* /*self*/) { return 0.0; }
void* bro_Gamepad_get_vibrationActuator(void* /*self*/) { return nullptr; }

// GamepadEvent
void* bro_GamepadEvent_create(void) { return nullptr; }
void bro_GamepadEvent_destroy(void* /*self*/) {}
void* bro_GamepadEvent_get_gamepad(void* /*self*/) { return nullptr; }

} // extern "C"
