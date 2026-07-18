#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::platform { class EventLoop; }

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

/// Install window.close() — must be called after event loop is created.
void installWindowClose(JSContext* ctx, platform::EventLoop* eventLoop);

} // namespace bro::js
