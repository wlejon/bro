#pragma once

#include "dom/node_handle.h"

namespace bro::dom { class Element; }

namespace bro::engine {

/// HTML5 drag and drop between elements — the `draggable` attribute and the
/// dragstart/drag/dragenter/dragover/dragleave/drop/dragend sequence.
class DragDrop {
public:
    /// A press landed on `target`. Records the nearest draggable ancestor.
    void arm(dom::Element* target, float x, float y);

    /// A move while the button is held. Starts the drag once past the threshold.
    bool update(dom::Element* under, float x, float y, int buttons);

    /// The button came up. Fires `drop` when the target accepted, then `dragend`.
    bool finish(dom::Element* under, float x, float y);

    /// Drop everything (app reload, window loss, escape).
    void cancel();

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
