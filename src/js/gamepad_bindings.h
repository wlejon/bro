#pragma once

// W3C-style Gamepad API: navigator.getGamepads() snapshots plus the `gamepad`
// payload of the window gamepadconnected/gamepaddisconnected events. State is
// owned by the engine (see engine/gamepad.h); this layer only shapes it.

#include <quickjs.h>

namespace bro::engine { class Engine; struct GamepadState; }

namespace bro::js {

/// Install navigator.getGamepads(). Call after installWindowBindings so the
/// navigator object exists.
void installGamepadBindings(JSContext* ctx, engine::Engine* engine);

/// Build a W3C Gamepad snapshot object (id, index, connected, mapping,
/// buttons, axes, timestamp, vibrationActuator) from an engine slot. Poll
/// semantics: the object is a plain copy, never live-updated.
JSValue buildGamepadSnapshot(JSContext* ctx, engine::Engine* engine,
                             const engine::GamepadState& gp);

} // namespace bro::js
