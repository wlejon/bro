#pragma once

#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::engine { class Engine; }

namespace bro::js {

/// Install headless-specific globals: screenshot(), advanceTime(), flush(),
/// sleep(), assert(). Only call in headless mode.
void installHeadlessBindings(JSContext* ctx, engine::Engine* engine);

/// Install `globalThis.scriptArgs`: the command-line arguments left over after
/// the app directory and script path, in order.
///
/// Same name and shape as QuickJS's own `qjs` shell uses, so a script written
/// against either runs under both. Always installed, even when empty, so a
/// script can write `scriptArgs.includes('--force')` without a guard — the
/// alternative (an undefined global) pushes every caller into
/// `(globalThis.scriptArgs || [])`, which reads like defensive code but is
/// really just working around a missing binding.
///
/// Re-installable: a location.reload() swaps the JSContext, and the arguments
/// the process was started with do not change, so the new realm gets them too.
void installScriptArgs(JSContext* ctx, const std::vector<std::string>& args);

/// Install the canvas-snapshot global (screenshotCanvas) and stash the engine
/// pointer for it. Safe and intended for both windowed and headless modes —
/// screenshotCanvas reads the canvas's Skia surface directly and works on
/// both the GPU-backed (windowed) and raster (headless) surface backends.
void installCanvasSnapshotBinding(JSContext* ctx, engine::Engine* engine);

} // namespace bro::js
