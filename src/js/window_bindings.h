#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

/// Install window globals: window, navigator, location, history,
/// addEventListener/removeEventListener, and SPA history polyfill.
/// Must be called before DOM bindings.
void installWindowBindings(JSContext* ctx, int viewportWidth, int viewportHeight);

} // namespace bro::js
