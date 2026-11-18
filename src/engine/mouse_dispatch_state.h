#pragma once

#include "dom/node_handle.h"

namespace bro::engine {

// ---------------------------------------------------------------------------
// Per-document mouse dispatch state
// ---------------------------------------------------------------------------

/// Bookkeeping for a single document's mouse event stream: tracks the
/// mousedown target (so click only fires on matching mouseup) and rolling
/// double-click / contextmenu state. One instance per document participating
/// in input (one for the app doc, one per system panel doc).
struct MouseDispatchState {
    // Generation-checked handles: targets cached across events resolve to
    // nullptr once the node (or its whole document) is freed, so no reap
    // pass has to hunt these down.
    dom::ElementHandle mouseDownTarget;
    dom::ElementHandle lastClickTarget;
    double lastClickTimeMs = 0.0;
    float lastClickX = 0.0f;
    float lastClickY = 0.0f;
    int clickCount = 0;
};

} // namespace bro::engine
