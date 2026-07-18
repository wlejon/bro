#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::platform { class EventLoop; class Window; }

namespace bro::js {

/// Install window globals: window, navigator, location, history,
/// addEventListener/removeEventListener, and SPA history polyfill.
/// Must be called before DOM bindings.
/// `devicePixelRatio` is the OS display scale for the hosting window
/// (Window::getDisplayScale()); headless passes the default 1.0 so tests
/// stay deterministic across machines. The engine refreshes the global on
/// resize and display-scale-change events (see Engine::handleResize).
void installWindowBindings(JSContext* ctx, int viewportWidth, int viewportHeight,
                           double devicePixelRatio = 1.0);

/// Install bro.window.* — runtime window management (borderless, always-on-
/// top, resize limits, position, minimize/maximize/restore, state). App
/// realm only. `window` may be null (--no-gpu headless): queries return
/// pinned defaults and mutators no-op. In headless mode the state-affecting
/// ops (minimize/maximize/restore, setPosition) no-op so a test can never
/// disturb the hidden window, while flag/limit setters still round-trip.
void installBroWindowBindings(JSContext* ctx, platform::Window* window,
                              bool headless = false);

/// Install window.close() — must be called after event loop is created.
void installWindowClose(JSContext* ctx, platform::EventLoop* eventLoop);

} // namespace bro::js
