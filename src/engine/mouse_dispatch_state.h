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

    // What to put back if the click that follows this press is cancelled.
    //
    // A checkbox/radio takes its new checkedness on MOUSEDOWN (focusNewControl)
    // — that is what makes the box visibly tick while the button is held — but
    // HTML makes the toggle part of the click's activation behaviour, so a
    // click with preventDefault() called on it has to undo it again ("legacy-
    // canceled-activation behavior"). Recorded at press, consumed at release.
    dom::ElementHandle activationTarget;      // the input the press toggled
    bool activationWasChecked = false;        // its checkedness before the toggle
    // The group member that was checked before, for a radio. Unchecking the one
    // that gained the check is not enough to restore a radio group: the one that
    // lost it has to get it back, or the group ends up with nothing selected.
    dom::ElementHandle activationPrevRadio;
};

} // namespace bro::engine
