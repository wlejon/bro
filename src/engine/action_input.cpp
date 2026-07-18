// Engine action-binding input — the device-independent half of the
// bro.settings action system. One binding string vocabulary:
//
//   "w", "ArrowUp", " "     web KeyboardEvent.key values      (keyboard)
//   "mouse:left"            left/middle/right/x1/x2           (mouse buttons)
//   "gamepad:south"         W3C standard-layout button names  (gamepad buttons;
//                           the analog triggers press past 0.1)
//   "gamepad:leftx+"        stick axis + direction            (axis bindings;
//                           pressed past the action's deadzone, released with
//                           hysteresis at deadzone * kActionAxisReleaseFactor)
//
// This file owns: the shared "action" CustomEvent dispatch, mouse-button
// action edges, axis-direction edge detection, and the polled
// actionStrength()/actionPressed() surface behind
// bro.settings.getActionStrength()/isActionPressed().
//
// Per-device edge production stays with the device code (keyboard in
// input_handling.cpp, gamepad buttons in gamepad.cpp) — they all funnel into
// dispatchActionEventForKey() here.

#include "engine/engine.h"
#include "engine/gamepad.h"
#include "engine/settings.h"

#include "js/runtime.h"
#include "js/event_dispatch.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"

#include <algorithm>
#include <cstring>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Binding-string parsing
// ---------------------------------------------------------------------------

// "mouse:<name>" -> DOM MouseEvent.button index, -1 if not a mouse binding.
static int mouseBindingButton(const std::string& key) {
    if (key.rfind("mouse:", 0) != 0) return -1;
    const std::string name = key.substr(6);
    if (name == "left")   return 0;
    if (name == "middle") return 1;
    if (name == "right")  return 2;
    if (name == "x1")     return 3;   // back
    if (name == "x2")     return 4;   // forward
    return -1;
}

// MouseEvent.buttons bit for a DOM button index. Mirrors domButtonMask in
// input_handling.cpp (DOM swaps right (2) and middle (4) relative to 1<<n).
static int actionDomButtonMask(int domButton) {
    switch (domButton) {
        case 0: return 1;   // left
        case 1: return 4;   // middle
        case 2: return 2;   // right
        case 3: return 8;   // back
        case 4: return 16;  // forward
        default: return 0;
    }
}

// "gamepad:<axis><+|->" -> axis index + direction (0 = negative, 1 =
// positive). Returns false for anything else (including button names).
static bool parseAxisBinding(const std::string& key, int& axisOut, int& dirOut) {
    if (key.rfind("gamepad:", 0) != 0) return false;
    std::string name = key.substr(8);
    if (name.size() < 2) return false;
    char sign = name.back();
    if (sign != '+' && sign != '-') return false;
    int axis = gamepadAxisIndex(name.substr(0, name.size() - 1));
    if (axis < 0) return false;
    axisOut = axis;
    dirOut = (sign == '+') ? 1 : 0;
    return true;
}

// Deadzone-rescaled magnitude of an axis deflection along one direction.
static float rescaleAxisMagnitude(float axisValue, int dir, float deadzone) {
    float m = (dir == 1) ? std::max(axisValue, 0.0f) : std::max(-axisValue, 0.0f);
    return std::clamp((m - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
}

// DOM button index -> "mouse:<name>" tail.
static const char* mouseButtonBindingName(int domButton) {
    switch (domButton) {
        case 0: return "left";
        case 1: return "middle";
        case 2: return "right";
        case 3: return "x1";
        case 4: return "x2";
        default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Shared "action" event dispatch
// ---------------------------------------------------------------------------

void Engine::dispatchActionEventForKey(const std::string& key, const char* phase,
                                       float strength, int gamepadIndex) {
    if (!settings_ || !jsRuntime_ || !document_ || !document_->body()) return;
    std::string action = settings_->getActionForKey(key);
    if (action.empty()) return;

    JSContext* ctx = jsRuntime_->getContext();
    JSValue jsEvent = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, jsEvent, "type", JS_NewString(ctx, "action"));
    JS_SetPropertyStr(ctx, jsEvent, "bubbles", JS_NewBool(ctx, 1));
    JS_SetPropertyStr(ctx, jsEvent, "cancelable", JS_NewBool(ctx, 1));

    JSValue detail = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, detail, "action", JS_NewString(ctx, action.c_str()));
    JS_SetPropertyStr(ctx, detail, "phase", JS_NewString(ctx, phase));
    JS_SetPropertyStr(ctx, detail, "key", JS_NewString(ctx, key.c_str()));
    JS_SetPropertyStr(ctx, detail, "strength",
                      JS_NewFloat64(ctx, static_cast<double>(strength)));
    if (gamepadIndex >= 0) {
        JS_SetPropertyStr(ctx, detail, "gamepad", JS_NewInt32(ctx, gamepadIndex));
    }
    JS_SetPropertyStr(ctx, jsEvent, "detail", detail);

    dom::Event evt("action");
    evt.setIsTrusted(true);
    js::dispatchDomEvent(ctx, document_->body(), evt, jsEvent);
    JS_FreeValue(ctx, jsEvent);
}

// ---------------------------------------------------------------------------
// Mouse-button action edges
// ---------------------------------------------------------------------------

void Engine::dispatchMouseButtonAction(int domButton, bool down) {
    const char* name = mouseButtonBindingName(domButton);
    if (!name) return;
    const int mask = actionDomButtonMask(domButton);
    if (down) {
        if (actionMouseDownMask_ & mask) return;  // duplicate down
        std::string key = std::string("mouse:") + name;
        if (!settings_ || settings_->getActionForKey(key).empty()) return;
        actionMouseDownMask_ |= mask;
        dispatchActionEventForKey(key, "down", 1.0f);
    } else {
        if (!(actionMouseDownMask_ & mask)) return;  // never saw the down
        actionMouseDownMask_ &= ~mask;
        dispatchActionEventForKey(std::string("mouse:") + name, "up", 0.0f);
    }
}

// ---------------------------------------------------------------------------
// Gamepad axis-direction action edges (hysteresis latch per pad + direction)
// ---------------------------------------------------------------------------

void Engine::evaluateAxisActions(GamepadState& gp, int w3cAxis) {
    if (!settings_) return;
    const char* axisName = gamepadAxisName(w3cAxis);
    if (!axisName) return;

    const float v = gp.axes[w3cAxis];
    for (int dir = 0; dir < 2; dir++) {
        std::string key = std::string("gamepad:") + axisName + (dir ? "+" : "-");
        std::string action = settings_->getActionForKey(key);
        if (action.empty()) {
            gp.axisActionPressed[w3cAxis][dir] = false;
            continue;
        }
        const float m = (dir == 1) ? std::max(v, 0.0f) : std::max(-v, 0.0f);
        const float dz = settings_->getActionDeadzone(action);
        const bool was = gp.axisActionPressed[w3cAxis][dir];
        // Hysteresis: press at >= deadzone, release only below
        // deadzone * kActionAxisReleaseFactor so threshold jitter can't
        // spam down/up pairs.
        const bool now = was ? (m >= dz * kActionAxisReleaseFactor) : (m >= dz);
        if (now == was) continue;
        gp.axisActionPressed[w3cAxis][dir] = now;
        const float strength = now ? rescaleAxisMagnitude(v, dir, dz) : 0.0f;
        dispatchActionEventForKey(key, now ? "down" : "up", strength, gp.index);
    }
}

// ---------------------------------------------------------------------------
// Polled action state
// ---------------------------------------------------------------------------

float Engine::actionStrength(const std::string& action) const {
    if (!settings_) return 0.0f;
    float best = 0.0f;
    for (const auto& key : settings_->getKeysForAction(action)) {
        // Mouse button: 0/1.
        int domButton = mouseBindingButton(key);
        if (domButton >= 0) {
            if (pressedButtons_ & actionDomButtonMask(domButton)) best = 1.0f;
            continue;
        }
        // Gamepad axis direction: deadzone-rescaled deflection, max over pads.
        int axis = 0, dir = 0;
        if (parseAxisBinding(key, axis, dir)) {
            const float dz = settings_->getActionDeadzone(action);
            for (const auto& gp : gamepads_) {
                if (!gp.connected) continue;
                best = std::max(best, rescaleAxisMagnitude(gp.axes[axis], dir, dz));
            }
            continue;
        }
        // Gamepad button: analog value (0/1 digital, analog triggers).
        if (key.rfind("gamepad:", 0) == 0) {
            int button = gamepadButtonIndex(key.substr(8));
            if (button >= 0) {
                for (const auto& gp : gamepads_) {
                    if (!gp.connected) continue;
                    best = std::max(best, gp.buttons[button]);
                }
            }
            continue;
        }
        // Keyboard: held -> 1.
        for (const auto& [keycode, webKey] : heldKeys_) {
            if (webKey == key) { best = 1.0f; break; }
        }
    }
    return best;
}

bool Engine::actionPressed(const std::string& action) const {
    if (!settings_) return false;
    for (const auto& key : settings_->getKeysForAction(action)) {
        int domButton = mouseBindingButton(key);
        if (domButton >= 0) {
            if (pressedButtons_ & actionDomButtonMask(domButton)) return true;
            continue;
        }
        int axis = 0, dir = 0;
        if (parseAxisBinding(key, axis, dir)) {
            // Use the hysteresis latch so polling agrees with the event edges.
            for (const auto& gp : gamepads_) {
                if (gp.connected && gp.axisActionPressed[axis][dir]) return true;
            }
            continue;
        }
        if (key.rfind("gamepad:", 0) == 0) {
            int button = gamepadButtonIndex(key.substr(8));
            if (button >= 0) {
                for (const auto& gp : gamepads_) {
                    if (gp.connected &&
                        gp.buttons[button] >= kGamepadTriggerPressThreshold)
                        return true;
                }
            }
            continue;
        }
        for (const auto& [keycode, webKey] : heldKeys_) {
            if (webKey == key) return true;
        }
    }
    return false;
}

} // namespace bro::engine
