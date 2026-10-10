// Engine gamepad handling — platform gamepad events in, W3C-standard-layout
// state out. These are Engine member function implementations (same split style
// as input_handling.cpp). The JS surface (navigator.getGamepads() snapshots,
// connection events' `gamepad` payload) is built by js/gamepad_bindings.cpp
// from the GamepadState slots owned here.
//
// Two producers feed the same path:
//   - real hardware: EventLoop's onGamepad* callbacks reach the
//     handleGamepad*() methods (windowed mode's frame loop pumps them);
//   - the headless simulation seam: gamepadConnectVirtual() & friends inject
//     below the JS API and above the platform, so tests exercise the identical
//     slot, snapshot, event, and action-dispatch code without hardware.

#include "engine/engine.h"
#include "engine/gamepad.h"
#include "engine/settings.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "platform/gamepads.h"
#include "util/time.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::engine {

// ---------------------------------------------------------------------------
// W3C standard-layout name tables
// ---------------------------------------------------------------------------

// Indexed by W3C button index. Names follow SDL's mapping-string fields
// (leftshoulder, dpup, back, guide, ...) — the strings apps bind with in
// bro.settings ("gamepad:south").
static const char* kButtonNames[kGamepadButtonCount] = {
    "south", "east", "west", "north",
    "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
    "back", "start", "leftstick", "rightstick",
    "dpup", "dpdown", "dpleft", "dpright", "guide",
};

static const char* kAxisNames[kGamepadAxisCount] = {
    "leftx", "lefty", "rightx", "righty",
};

const char* gamepadButtonName(int w3cIndex) {
    if (w3cIndex < 0 || w3cIndex >= kGamepadButtonCount) return nullptr;
    return kButtonNames[w3cIndex];
}

int gamepadButtonIndex(const std::string& name) {
    for (int i = 0; i < kGamepadButtonCount; i++)
        if (name == kButtonNames[i]) return i;
    return -1;
}

const char* gamepadAxisName(int w3cIndex) {
    if (w3cIndex < 0 || w3cIndex >= kGamepadAxisCount) return nullptr;
    return kAxisNames[w3cIndex];
}

int gamepadAxisIndex(const std::string& name) {
    for (int i = 0; i < kGamepadAxisCount; i++)
        if (name == kAxisNames[i]) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Platform -> W3C layout mapping. The platform's gamepad layer already normalizes
// every device to one logical layout, so this is a fixed table, not per-device.
// ---------------------------------------------------------------------------

static int platformButtonToW3C(int button) {
    using B = platform::GamepadButton;
    switch (static_cast<B>(button)) {
        case B::South:          return 0;
        case B::East:           return 1;
        case B::West:           return 2;
        case B::North:          return 3;
        case B::LeftShoulder:   return 4;
        case B::RightShoulder:  return 5;
        // 6/7 (triggers) arrive as axes; see handleGamepadAxis.
        case B::Back:           return 8;
        case B::Start:          return 9;
        case B::LeftStick:      return 10;
        case B::RightStick:     return 11;
        case B::DpadUp:         return 12;
        case B::DpadDown:       return 13;
        case B::DpadLeft:       return 14;
        case B::DpadRight:      return 15;
        case B::Guide:          return 16;
        default: return -1;  // misc/paddles/touchpad: not in the standard layout
    }
}

// ---------------------------------------------------------------------------
// Slot management
// ---------------------------------------------------------------------------

GamepadState* Engine::gamepadByInstance(uint32_t instanceId) {
    if (instanceId == 0) return nullptr;  // 0 marks virtual pads
    for (auto& gp : gamepads_)
        if (gp.connected && gp.instanceId == instanceId) return &gp;
    return nullptr;
}

GamepadState* Engine::connectedGamepadAt(int index) {
    if (index < 0 || index >= static_cast<int>(gamepads_.size())) return nullptr;
    GamepadState& gp = gamepads_[static_cast<size_t>(index)];
    return gp.connected ? &gp : nullptr;
}

GamepadState& Engine::allocateGamepadSlot() {
    // W3C contract: an index is stable for a device's lifetime, and the first
    // free (previously vacated) slot is reused by the next arrival.
    for (auto& gp : gamepads_) {
        if (!gp.connected) {
            int index = gp.index;
            gp = GamepadState{};
            gp.index = index;
            return gp;
        }
    }
    GamepadState gp;
    gp.index = static_cast<int>(gamepads_.size());
    gamepads_.push_back(std::move(gp));
    return gamepads_.back();
}

// ---------------------------------------------------------------------------
// Connection events + action dispatch
// ---------------------------------------------------------------------------

void Engine::dispatchGamepadConnectionEvent(const GamepadState& gp, bool connected) {
    const char* type = connected ? "gamepadconnected" : "gamepaddisconnected";
    dom::CustomEvent evt(type);
    evt.setDetail(std::to_string(gp.index));
    evt.setIsTrusted(true);
    dispatchWindowEvent(evt);
}

// A button's analog value changed. Updates the slot, and on a press/release
// edge dispatches the same "action" CustomEvent the keyboard path emits
// (dispatchActionEventForKey in action_input.cpp) when the button is bound
// via bro.settings — binding strings are "gamepad:<name>", e.g.
// "gamepad:south". detail.strength carries the button's analog value at the
// edge (1/0 for digital buttons, the trigger's analog value for 6/7).
void Engine::gamepadButtonChanged(GamepadState& gp, int w3cIndex, float value) {
    if (w3cIndex < 0 || w3cIndex >= kGamepadButtonCount) return;
    value = std::clamp(value, 0.0f, 1.0f);
    const bool wasPressed = gp.buttons[w3cIndex] >= kGamepadTriggerPressThreshold;
    const bool pressed = value >= kGamepadTriggerPressThreshold;
    if (gp.buttons[w3cIndex] == value) return;
    gp.buttons[w3cIndex] = value;
    gp.timestampMs = util::currentTimeMs();
    if (pressed == wasPressed) return;  // analog-only change, no edge

    std::string key = std::string("gamepad:") + kButtonNames[w3cIndex];
    dispatchActionEventForKey(key, pressed ? "down" : "up", value, gp.index);
}

// A stick axis moved (real SDL path and the headless virtual-axis seam both
// land here): update the slot, then run the "gamepad:<axis>+/-" action
// bindings' edge detection so injected and real axes drive identical
// dispatch.
void Engine::gamepadAxisChanged(GamepadState& gp, int w3cAxis, float value) {
    if (w3cAxis < 0 || w3cAxis >= kGamepadAxisCount) return;
    value = std::clamp(value, -1.0f, 1.0f);
    if (gp.axes[w3cAxis] == value) return;
    gp.axes[w3cAxis] = value;
    gp.timestampMs = util::currentTimeMs();
    evaluateAxisActions(gp, w3cAxis);
}

// ---------------------------------------------------------------------------
// Device event path (called from the EventLoop callbacks; windowed frame loop)
// ---------------------------------------------------------------------------

void Engine::useGamepads() {
    if (gamepadsStarted_) return;
    gamepadsStarted_ = true;
    // A dedicated server has no window system to read them through.
    if (displayMode_ == DisplayMode::Server) return;
    platform::gamepads().start();
}

void Engine::handleGamepadAdded(uint32_t instanceId) {
    if (gamepadByInstance(instanceId)) return;  // already open (duplicate event)
    std::string err;
    if (!platform::gamepads().open(instanceId, &err)) {
        LOG_WARN("Gamepad %u: opening the gamepad failed: %s", instanceId, err.c_str());
        return;
    }
    GamepadState& gp = allocateGamepadSlot();
    gp.instanceId = instanceId;
    gp.opened = true;
    std::string name = platform::gamepads().name(instanceId);
    gp.id = name.empty() ? "Gamepad" : std::move(name);
    gp.connected = true;
    gp.timestampMs = util::currentTimeMs();
    LOG_INFO("Gamepad connected: \"%s\" (slot %d)", gp.id.c_str(), gp.index);
    dispatchGamepadConnectionEvent(gp, true);
}

void Engine::handleGamepadRemoved(uint32_t instanceId) {
    GamepadState* gp = gamepadByInstance(instanceId);
    if (!gp) return;
    if (gp->opened) {
        platform::gamepads().close(gp->instanceId);
        gp->opened = false;
    }
    gp->connected = false;
    gp->timestampMs = util::currentTimeMs();
    LOG_INFO("Gamepad disconnected: \"%s\" (slot %d)", gp->id.c_str(), gp->index);
    dispatchGamepadConnectionEvent(*gp, false);
}

void Engine::handleGamepadButton(uint32_t instanceId, int button, bool down) {
    noteUserActivity();
    GamepadState* gp = gamepadByInstance(instanceId);
    if (!gp) return;
    int w3c = platformButtonToW3C(button);
    if (w3c < 0) return;
    gamepadButtonChanged(*gp, w3c, down ? 1.0f : 0.0f);
}

void Engine::handleGamepadAxis(uint32_t instanceId, int axis, float value) {
    using A = platform::GamepadAxis;
    GamepadState* gp = gamepadByInstance(instanceId);
    if (!gp) return;
    switch (static_cast<A>(axis)) {
        case A::LeftX:  case A::LeftY:
        case A::RightX: case A::RightY: {
            int w3c = axis - static_cast<int>(A::LeftX);  // enum values are contiguous
            gamepadAxisChanged(*gp, w3c, value);
            break;
        }
        // Triggers are axes on the wire but buttons 6/7 in the W3C layout.
        case A::LeftTrigger:
            gamepadButtonChanged(*gp, 6, value);
            break;
        case A::RightTrigger:
            gamepadButtonChanged(*gp, 7, value);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Virtual pads (headless simulation seam)
// ---------------------------------------------------------------------------

int Engine::gamepadConnectVirtual(const std::string& id) {
    GamepadState& gp = allocateGamepadSlot();
    gp.instanceId = 0;
    gp.opened = false;
    gp.id = id.empty() ? "Virtual Gamepad (bro)" : id;
    gp.connected = true;
    gp.virtualPad = true;
    gp.timestampMs = util::currentTimeMs();
    int index = gp.index;
    dispatchGamepadConnectionEvent(gp, true);
    return index;
}

bool Engine::gamepadDisconnectVirtual(int index) {
    GamepadState* gp = connectedGamepadAt(index);
    if (!gp || !gp->virtualPad) return false;
    gp->connected = false;
    gp->timestampMs = util::currentTimeMs();
    dispatchGamepadConnectionEvent(*gp, false);
    return true;
}

bool Engine::gamepadSetVirtualButton(int index, int w3cButton, bool pressed, float value) {
    GamepadState* gp = connectedGamepadAt(index);
    if (!gp || !gp->virtualPad) return false;
    if (w3cButton < 0 || w3cButton >= kGamepadButtonCount) return false;
    if (value < 0.0f) value = pressed ? 1.0f : 0.0f;  // no explicit analog value
    gamepadButtonChanged(*gp, w3cButton, value);
    return true;
}

bool Engine::gamepadSetVirtualAxis(int index, int w3cAxis, float value) {
    GamepadState* gp = connectedGamepadAt(index);
    if (!gp || !gp->virtualPad) return false;
    if (w3cAxis < 0 || w3cAxis >= kGamepadAxisCount) return false;
    // Same path as real SDL axis motion — axis-direction action bindings and
    // hysteresis run identically, so headless tests can assert exact edges
    // and strengths.
    gamepadAxisChanged(*gp, w3cAxis, value);
    return true;
}

// ---------------------------------------------------------------------------
// Rumble
// ---------------------------------------------------------------------------

bool Engine::gamepadRumble(int index, float strongMagnitude, float weakMagnitude,
                           int durationMs) {
    GamepadState* gp = connectedGamepadAt(index);
    if (!gp) return false;
    strongMagnitude = std::clamp(strongMagnitude, 0.0f, 1.0f);
    weakMagnitude = std::clamp(weakMagnitude, 0.0f, 1.0f);
    durationMs = std::max(0, durationMs);
    gp->rumbleStrong = strongMagnitude;
    gp->rumbleWeak = weakMagnitude;
    gp->rumbleDurationMs = durationMs;
    if (gp->opened) {
        return platform::gamepads().rumble(gp->instanceId, strongMagnitude, weakMagnitude,
                                           durationMs);
    }
    return true;  // virtual pad: recorded above, nothing to drive
}

bool Engine::gamepadRumbleTriggers(int index, float leftMagnitude,
                                   float rightMagnitude, int durationMs) {
    GamepadState* gp = connectedGamepadAt(index);
    if (!gp) return false;
    leftMagnitude = std::clamp(leftMagnitude, 0.0f, 1.0f);
    rightMagnitude = std::clamp(rightMagnitude, 0.0f, 1.0f);
    durationMs = std::max(0, durationMs);
    gp->rumbleLeftTrigger = leftMagnitude;
    gp->rumbleRightTrigger = rightMagnitude;
    gp->rumbleTriggerDurationMs = durationMs;
    if (gp->opened) {
        return platform::gamepads().rumbleTriggers(gp->instanceId, leftMagnitude,
                                                   rightMagnitude, durationMs);
    }
    return true;  // virtual pad: recorded above, nothing to drive
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void Engine::closeAllGamepads() {
    for (auto& gp : gamepads_) {
        if (gp.opened) {
            platform::gamepads().close(gp.instanceId);
            gp.opened = false;
        }
        gp.connected = false;
    }
}

} // namespace bro::engine
