#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::engine { class Engine; }

namespace bro::js {

/// Install headless-specific globals: screenshot(), advanceTime(), flush(),
/// sleep(), assert(). Only call in headless mode.
void installHeadlessBindings(JSContext* ctx, engine::Engine* engine);

} // namespace bro::js
