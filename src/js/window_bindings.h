#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::platform { class EventLoop; }

namespace bro::js {

/// Install window globals: window, navigator, location, history,
/// addEventListener/removeEventListener, and SPA history polyfill.
/// Must be called before DOM bindings.
void installWindowBindings(JSContext* ctx, int viewportWidth, int viewportHeight);

/// Install window.close() — must be called after event loop is created.
void installWindowClose(JSContext* ctx, platform::EventLoop* eventLoop);

} // namespace bro::js
