#pragma once

#include "dom/node_handle.h"

#include <quickjs.h>

namespace bro::dom { class Element; }

namespace bro::engine {

/// HTML5 drag and drop between elements — the `draggable` attribute and the
/// dragstart/drag/dragenter/dragover/dragleave/drop/dragend sequence.
///
/// This is the mechanism a page uses to let the user rearrange its own things:
/// reorder a list, reparent a tree node, drop a layer onto another. It is
/// distinct from the OS file drop (`dropFiles`, docs/file-api.js), which
/// arrives from outside the window and starts at `dragenter`; this one starts
/// inside the page, on a mouse press over a `draggable` element.
///
/// The state machine lives in the Engine's mouse handlers:
///   press   → arm()      remember a draggable candidate under the pointer
///   move    → update()   past the threshold, fire dragstart and then track
///   release → finish()   drop if the target accepted, then dragend
///
/// A drag is only *allowed to drop* where a `dragover` (or `dragenter`) called
/// preventDefault() — that inversion is the spec's, and the reason a handler
/// that forgets it sees no `drop`.
class DragDrop {
public:
    /// A press landed on `target`. Records the nearest draggable ancestor, if
    /// any, so a later move can start a drag. Cheap when nothing is draggable.
    void arm(dom::Element* target, float x, float y);

    /// A move while the button is held. Starts the drag once past the
    /// threshold, then dispatches drag / dragenter / dragleave / dragover.
    /// Returns true while a drag is in progress, which tells the caller to
    /// skip the ordinary hover/selection work for this move.
    bool update(JSContext* ctx, dom::Element* under, float x, float y, int buttons);

    /// The button came up. Fires `drop` when the target accepted, then
    /// `dragend`. Returns true if a drag was in progress — the caller
    /// suppresses the click that would otherwise follow.
    bool finish(JSContext* ctx, dom::Element* under, float x, float y);

    /// Drop everything (app reload, window loss, escape).
    void cancel(JSContext* ctx);

    bool dragging() const { return active_; }

private:
    dom::ElementHandle candidate_;   // draggable element under the press
    dom::ElementHandle source_;      // the element actually being dragged
    dom::ElementHandle target_;      // element the pointer is over now
    float startX_ = 0.0f, startY_ = 0.0f;
    bool armed_ = false;
    bool active_ = false;
    bool dropAllowed_ = false;
};

} // namespace bro::engine
