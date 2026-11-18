#pragma once

// The web globals installed by a file of their own and shared with nothing
// else: storage, gamepad, navigator, performance, the platform odds and ends,
// CSS, brand constructors, document.fonts, and the video encoders.

#include "embed/embed.h"

#include <functional>

namespace bro::dom { class Document; }

namespace bro::engine {
struct GamepadState;
class Engine;
}  // namespace bro::engine

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

class HostClass;

// ---------------------------------------------------------------------------
// Storage & Gamepad (dom_storage.cpp / dom_gamepad.cpp)
// ---------------------------------------------------------------------------
Value makeLocalStorageValue();
Value makeScreenValue();
Value makeNavigatorValue();
// document.fonts over the document `docOf` answers (host_font_face_set.cpp).
Value makeFontFaceSet(std::function<dom::Document*()> docOf);
Value buildGamepadSnapshot(const engine::GamepadState& gp);
void installGamepadButtonGlobals();
const HostClass& gamepadEventHostClass();

// `navigator` as a host global (host_navigator.cpp): makeNavigatorValue's
// object plus `clipboard` and `getBattery()`.
void installNavigatorGlobal();

// `performance` (host_performance.cpp): `now()` on hostClockMs, and the User
// Timing marks and measures on the same clock.
Value makePerformanceValue();

// ---------------------------------------------------------------------------
// Platform odds and ends (host_platform.cpp)
// ---------------------------------------------------------------------------

// queueMicrotask, screen, alert/confirm/prompt, and the DOM interface NAMES
// libraries test for (`typeof Node !== "undefined"`,
// `x instanceof HTMLInputElement`). No state, no frame seam.
void installPlatformGlobals();
// The frame pump that settles show*DialogAsync's promises.
void installDialogPump(engine::Engine& engine);

// The `CSS` namespace: supports() and escape() (host_css_namespace.cpp).
void installCssNamespace();

// Brand constructors (dom_globals.cpp)
Value makeBrandConstructor(const char* name);

// VideoEncoder / GifEncoder (host_video.cpp)
void installVideoGlobals();

}  // namespace bro::bronze_host
