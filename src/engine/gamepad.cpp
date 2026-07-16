// Engine gamepad handling — SDL gamepad events in, W3C-standard-layout state
// out. These are Engine member function implementations (same split style as
// input_handling.cpp). The JS surface (navigator.getGamepads() snapshots,
// connection events' `gamepad` payload) is built by js/gamepad_bindings.cpp
// from the GamepadState slots owned here.
//
// Two producers feed the same path:
//   - real hardware: EventLoop forwards SDL_EVENT_GAMEPAD_* to the
//     handleGamepad*() methods (windowed mode's frame loop pumps them);
//   - the headless simulation seam: gamepadConnectVirtual() & friends inject
//     below the JS API and above SDL, so tests exercise the identical slot,
//     snapshot, event, and action-dispatch code without hardware.

#include "engine/engine.h"
#include "engine/gamepad.h"
#include "engine/settings.h"

#include "js/runtime.h"
#include "js/event_dispatch.h"
#include "js/gamepad_bindings.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "util/time.h"
#include "util/log.h"

#include <SDL3/SDL.h>
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
// SDL -> W3C layout mapping. SDL's gamepad abstraction already normalizes
// every device to one logical layout, so this is a fixed table, not per-device.
// ---------------------------------------------------------------------------

static int sdlButtonToW3C(int sdlButton) {
    switch (sdlButton) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return 0;
        case SDL_GAMEPAD_BUTTON_EAST:           return 1;
        case SDL_GAMEPAD_BUTTON_WEST:           return 2;
        case SDL_GAMEPAD_BUTTON_NORTH:          return 3;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return 4;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 5;
        // 6/7 (triggers) arrive as SDL axes; see handleGamepadAxis.
        case SDL_GAMEPAD_BUTTON_BACK:           return 8;
        case SDL_GAMEPAD_BUTTON_START:          return 9;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return 10;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return 11;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return 12;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return 13;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return 14;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return 15;
        case SDL_GAMEPAD_BUTTON_GUIDE:          return 16;
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
    if (!jsRuntime_) return;
    JSContext* ctx = jsRuntime_->getContext();
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue dispatch = JS_GetPropertyStr(ctx, global, "__bro_dispatch_window_event");
    if (JS_IsFunction(ctx, dispatch)) {
        const char* type = connected ? "gamepadconnected" : "gamepaddisconnected";
        JSValue evtType = JS_NewString(ctx, type);
        JSValue evt = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, evt, "type", JS_NewString(ctx, type));
        JS_SetPropertyStr(ctx, evt, "gamepad", js::buildGamepadSnapshot(ctx, this, gp));
        JSValue args[2] = { evtType, evt };
        JSValue ret = JS_Call(ctx, dispatch, global, 2, args);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, evtType);
        JS_FreeValue(ctx, evt);
    }
    JS_FreeValue(ctx, dispatch);
    JS_FreeValue(ctx, global);
    jsRuntime_->executePendingJobs();
}

// A button's analog value changed. Updates the slot, and on a press/release
// edge dispatches the same "action" CustomEvent the keyboard path emits (see
// dispatchActionEvent in input_handling.cpp) when the button is bound via
// bro.settings — binding strings are "gamepad:<name>", e.g. "gamepad:south".
void Engine::gamepadButtonChanged(GamepadState& gp, int w3cIndex, float value) {
    if (w3cIndex < 0 || w3cIndex >= kGamepadButtonCount) return;
    value = std::clamp(value, 0.0f, 1.0f);
    const bool wasPressed = gp.buttons[w3cIndex] >= kGamepadTriggerPressThreshold;
    const bool pressed = value >= kGamepadTriggerPressThreshold;
    if (gp.buttons[w3cIndex] == value) return;
    gp.buttons[w3cIndex] = value;
    gp.timestampMs = util::currentTimeMs();
    if (pressed == wasPressed) return;  // analog-only change, no edge

    if (!settings_ || !jsRuntime_ || !document_ || !document_->body()) return;
    std::string key = std::string("gamepad:") + kButtonNames[w3cIndex];
    std::string action = settings_->getActionForKey(key);
    if (action.empty()) return;

    JSContext* ctx = jsRuntime_->getContext();
    JSValue jsEvent = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, jsEvent, "type", JS_NewString(ctx, "action"));
    JS_SetPropertyStr(ctx, jsEvent, "bubbles", JS_NewBool(ctx, 1));
    JS_SetPropertyStr(ctx, jsEvent, "cancelable", JS_NewBool(ctx, 1));

    JSValue detail = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, detail, "action", JS_NewString(ctx, action.c_str()));
    JS_SetPropertyStr(ctx, detail, "phase", JS_NewString(ctx, pressed ? "down" : "up"));
    JS_SetPropertyStr(ctx, detail, "key", JS_NewString(ctx, key.c_str()));
    JS_SetPropertyStr(ctx, detail, "gamepad", JS_NewInt32(ctx, gp.index));
    JS_SetPropertyStr(ctx, jsEvent, "detail", detail);

    dom::Event evt("action");
    evt.setIsTrusted(true);
    js::dispatchDomEvent(ctx, document_->body(), evt, jsEvent);
    JS_FreeValue(ctx, jsEvent);
}

// ---------------------------------------------------------------------------
// SDL event path (called from the EventLoop callbacks; windowed frame loop)
// ---------------------------------------------------------------------------

void Engine::handleGamepadAdded(uint32_t instanceId) {
    if (gamepadByInstance(instanceId)) return;  // already open (duplicate event)
    SDL_Gamepad* handle = SDL_OpenGamepad(instanceId);
    if (!handle) {
        LOG_WARN("Gamepad %u: SDL_OpenGamepad failed: %s", instanceId, SDL_GetError());
        return;
    }
    GamepadState& gp = allocateGamepadSlot();
    gp.instanceId = instanceId;
    gp.handle = handle;
    const char* name = SDL_GetGamepadName(handle);
    gp.id = name ? name : "Gamepad";
    gp.connected = true;
    gp.timestampMs = util::currentTimeMs();
    LOG_INFO("Gamepad connected: \"%s\" (slot %d)", gp.id.c_str(), gp.index);
    dispatchGamepadConnectionEvent(gp, true);
}

void Engine::handleGamepadRemoved(uint32_t instanceId) {
    GamepadState* gp = gamepadByInstance(instanceId);
    if (!gp) return;
    if (gp->handle) {
        SDL_CloseGamepad(gp->handle);
        gp->handle = nullptr;
    }
    gp->connected = false;
    gp->timestampMs = util::currentTimeMs();
    LOG_INFO("Gamepad disconnected: \"%s\" (slot %d)", gp->id.c_str(), gp->index);
    dispatchGamepadConnectionEvent(*gp, false);
}

void Engine::handleGamepadButton(uint32_t instanceId, int sdlButton, bool down) {
    GamepadState* gp = gamepadByInstance(instanceId);
    if (!gp) return;
    int w3c = sdlButtonToW3C(sdlButton);
    if (w3c < 0) return;
    gamepadButtonChanged(*gp, w3c, down ? 1.0f : 0.0f);
}

void Engine::handleGamepadAxis(uint32_t instanceId, int sdlAxis, float value) {
    GamepadState* gp = gamepadByInstance(instanceId);
    if (!gp) return;
    switch (sdlAxis) {
        case SDL_GAMEPAD_AXIS_LEFTX:  case SDL_GAMEPAD_AXIS_LEFTY:
        case SDL_GAMEPAD_AXIS_RIGHTX: case SDL_GAMEPAD_AXIS_RIGHTY: {
            int w3c = sdlAxis - SDL_GAMEPAD_AXIS_LEFTX;  // enum values are contiguous
            gp->axes[w3c] = std::clamp(value, -1.0f, 1.0f);
            gp->timestampMs = util::currentTimeMs();
            break;
        }
        // Triggers are axes on the wire but buttons 6/7 in the W3C layout.
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
            gamepadButtonChanged(*gp, 6, value);
            break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
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
    gp.handle = nullptr;
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
    gp->axes[w3cAxis] = std::clamp(value, -1.0f, 1.0f);
    gp->timestampMs = util::currentTimeMs();
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
    if (gp->handle) {
        return SDL_RumbleGamepad(gp->handle,
                                 static_cast<Uint16>(strongMagnitude * 0xFFFF),
                                 static_cast<Uint16>(weakMagnitude * 0xFFFF),
                                 static_cast<Uint32>(durationMs));
    }
    return true;  // virtual pad: recorded above, nothing to drive
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------

void Engine::closeAllGamepads() {
    for (auto& gp : gamepads_) {
        if (gp.handle) {
            SDL_CloseGamepad(gp.handle);
            gp.handle = nullptr;
        }
        gp.connected = false;
    }
}

} // namespace bro::engine
