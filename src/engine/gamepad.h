#pragma once

// Gamepad state shared between the engine input layer and the JS bindings.
// Deliberately SDL-header-free (engine.h includes this): SDL_Gamepad is an
// opaque struct so a plain forward declaration suffices.

#include <cstdint>
#include <string>

typedef struct SDL_Gamepad SDL_Gamepad;

namespace bro::engine {

// W3C "standard" gamepad layout — 17 buttons, 4 axes.
//   buttons: 0 south  1 east  2 west  3 north  4 leftshoulder  5 rightshoulder
//            6 lefttrigger  7 righttrigger  8 back  9 start  10 leftstick
//            11 rightstick  12 dpup  13 dpdown  14 dpleft  15 dpright  16 guide
//   axes:    0 leftx  1 lefty  2 rightx  3 righty  (all -1..1)
inline constexpr int kGamepadButtonCount = 17;
inline constexpr int kGamepadAxisCount = 4;

/// One controller slot. Slots are stable for a device's lifetime (that's the
/// W3C Gamepad.index contract); a disconnected slot stays in the list with
/// connected=false and is reused by the next device that arrives.
struct GamepadState {
    int index = 0;                  // W3C Gamepad.index (slot position)
    uint32_t instanceId = 0;        // SDL joystick instance id (0 = virtual pad)
    SDL_Gamepad* handle = nullptr;  // open SDL handle; null for virtual pads
    std::string id;                 // W3C Gamepad.id (device name)
    bool connected = false;
    bool virtualPad = false;        // injected via the headless seam, no SDL device
    float buttons[kGamepadButtonCount] = {};  // analog value 0..1 per button
    float axes[kGamepadAxisCount] = {};       // -1..1
    double timestampMs = 0.0;       // last state change (engine wall clock)
    // Last rumble request. SDL only sees it for real pads; recorded for all
    // so the headless seam can observe what an app asked for.
    float rumbleStrong = 0.0f;
    float rumbleWeak = 0.0f;
    int rumbleDurationMs = 0;
    // Last trigger-rumble request (vibrationActuator "trigger-rumble").
    float rumbleLeftTrigger = 0.0f;
    float rumbleRightTrigger = 0.0f;
    int rumbleTriggerDurationMs = 0;
};

/// A button reads as pressed once its analog value crosses this (matters for
/// the trigger buttons 6/7, which are axes on the wire).
inline constexpr float kGamepadTriggerPressThreshold = 0.1f;

/// Canonical button name for a W3C standard-layout index ("south", "start",
/// "dpup", ...). These match SDL's mapping-string field names and are the
/// names used in `bro.settings` bindings ("gamepad:south"). nullptr if out of
/// range.
const char* gamepadButtonName(int w3cIndex);

/// Inverse of gamepadButtonName. -1 if unknown.
int gamepadButtonIndex(const std::string& name);

/// Canonical axis name for a W3C axis index ("leftx".."righty"); nullptr if
/// out of range.
const char* gamepadAxisName(int w3cIndex);

/// Inverse of gamepadAxisName. -1 if unknown.
int gamepadAxisIndex(const std::string& name);

} // namespace bro::engine
