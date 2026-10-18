#pragma once

#include <vector>

namespace bro::engine {

// DomUndoStack's positions (engine/dom_undo.h), apart so a header that only
// stores a selection (engine_types.h) does not pull in the DOM.

// A DOM position as a child-index path from the host plus an offset in
// the resolved container. `valid` distinguishes "no position recorded"
// from "the host itself, offset 0".
struct DomUndoPos {
    std::vector<int> path;
    int offset = 0;
    bool valid = false;

    bool operator==(const DomUndoPos& o) const {
        return valid == o.valid && offset == o.offset && path == o.path;
    }
    bool operator!=(const DomUndoPos& o) const { return !(*this == o); }
};

// Anchor (pinned end) + focus (moving end), matching dom::Selection.
struct DomUndoSel {
    DomUndoPos anchor, focus;
    bool operator==(const DomUndoSel& o) const {
        return anchor == o.anchor && focus == o.focus;
    }
    bool operator!=(const DomUndoSel& o) const { return !(*this == o); }
};

} // namespace bro::engine
