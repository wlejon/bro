#pragma once

#include "embed/embed.h"
#include "engine/engine.h"
#include <span>

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// Mouse button mapping: DOM 0=left, 1=middle, 2=right -> SDL 1=left, 2=middle, 3=right
inline int domToSdlButton(int domButton) {
    switch (domButton) {
        case 0: return 1;
        case 1: return 2;
        case 2: return 3;
        default: return domButton + 1;
    }
}

// Convert viewport-relative y (from DOM getBoundingClientRect) to engine screen-space y
inline float toScreenY(engine::Engine* engine, double viewportY) {
    return static_cast<float>(viewportY) + static_cast<float>(engine->contentTop());
}

// Read optional windowId argument (index `idx` in args span)
inline uint64_t argWindowId(std::span<const Value> a, size_t idx) {
    if (idx >= a.size()) return 0;
    if (ev::isUndefined(a[idx]) || ev::isNull(a[idx])) return 0;
    double d = ev::toDouble(a[idx]);
    return d > 0.0 ? static_cast<uint64_t>(d) : 0;
}

// Input y for target window (main window reserves top menu inset, secondary doesn't)
inline float toWindowY(engine::Engine* engine, double y, uint64_t windowId) {
    return windowId ? static_cast<float>(y) : toScreenY(engine, y);
}

void installHeadlessInput(engine::Engine& engine);
void installHeadlessFrame(engine::Engine& engine);
void installHeadlessTestHooks(engine::Engine& engine);

} // namespace bro::bronze_host
