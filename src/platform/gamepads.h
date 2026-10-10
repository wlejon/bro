#pragma once
// Game controllers: devices come and go through EventLoop (onGamepadAdded /
// Removed / Button / Axis); this opens them and drives their rumble motors.
// Buttons and axes are named by position on a standard controller (the
// Xbox-style layout every mapping database normalises to).

#include <cstdint>
#include <string>

namespace bro::platform {

enum class GamepadButton : int {
    South = 0,      // A / Cross
    East,           // B / Circle
    West,           // X / Square
    North,          // Y / Triangle
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,          // share / capture / mute
    RightPaddle1,
    LeftPaddle1,
    RightPaddle2,
    LeftPaddle2,
    Touchpad,
};

enum class GamepadAxis : int {
    LeftX = 0,
    LeftY,
    RightX,
    RightY,
    LeftTrigger,
    RightTrigger,
};

class Gamepads {
public:
    virtual ~Gamepads() = default;

    /// Begin reading controllers; until then no device is announced. Finding
    /// them enumerates every HID device (~200 ms on Windows), so the engine
    /// starts it the first time a page asks about gamepads, never at startup.
    /// Idempotent.
    virtual void start() = 0;

    /// Open the device an onGamepadAdded announced. False when it cannot be
    /// opened (it vanished, or there is no controller backend); `error` then
    /// says why.
    virtual bool open(uint32_t instanceId, std::string* error = nullptr) = 0;
    virtual void close(uint32_t instanceId) = 0;
    /// The controller's product name ("" when unknown or not open).
    virtual std::string name(uint32_t instanceId) = 0;
    /// Run the low- (strong) and high-frequency (weak) motors, 0..1, for
    /// `durationMs`; zeros stop them. False when the pad cannot rumble.
    virtual bool rumble(uint32_t instanceId, float strong, float weak, int durationMs) = 0;
    /// The impulse-trigger motors, where the pad has them.
    virtual bool rumbleTriggers(uint32_t instanceId, float left, float right, int durationMs) = 0;
};

/// The active WindowSystem's gamepads.
Gamepads& gamepads();

} // namespace bro::platform
