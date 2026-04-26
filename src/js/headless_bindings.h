#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::engine { class Engine; }

namespace bro::js {

/// Install headless-specific globals: screenshot(), advanceTime(), flush(),
/// sleep(), assert(). Only call in headless mode.
void installHeadlessBindings(JSContext* ctx, engine::Engine* engine);

/// Install the canvas-snapshot global (screenshotCanvas) and stash the engine
/// pointer for it. Safe and intended for both windowed and headless modes —
/// screenshotCanvas reads the canvas's Skia surface directly and works on
/// both the GPU-backed (windowed) and raster (headless) surface backends.
void installCanvasSnapshotBinding(JSContext* ctx, engine::Engine* engine);

} // namespace bro::js
