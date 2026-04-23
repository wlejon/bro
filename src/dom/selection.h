#pragma once
#include "dom/range.h"
#include <string>

namespace bro::dom {

class Document;
class Node;

// Window.getSelection() — the user's selection (or a synthetic one set by
// script). Single-range for this implementation; spec allows multi-range but
// in practice only Firefox exposes it.
//
// Direction tracks which endpoint is the "anchor" (where the user pressed)
// versus the "focus" (where the cursor currently is). A backward direction
// means the anchor is at end_ and focus is at start_.
class Selection {
public:
    enum Direction : uint8_t { Forward, Backward, None };

    explicit Selection(Document* doc);
    ~Selection();

    // Endpoint accessors. When the selection is empty, anchor/focus are null.
    Node* anchorNode() const;
    int   anchorOffset() const;
    Node* focusNode() const;
    int   focusOffset() const;

    bool isCollapsed() const { return range_.collapsed() || !hasRange_; }
    int  rangeCount()  const { return hasRange_ ? 1 : 0; }
    const Range* getRangeAt(int index) const;
    Range*       getRangeAt(int index);
    std::string  type() const; // "None" | "Caret" | "Range"
    std::string  toString() const;

    // Mutators. These update the range and schedule a selectionchange on the
    // document.
    void setRange(Node* start, int startOff, Node* end, int endOff,
                  Direction dir = Forward);
    void addRange(const Range& r);
    void removeRange(const Range& r);
    void removeAllRanges();
    void empty() { removeAllRanges(); }
    void collapse(Node* node, int offset);
    void collapseToStart();
    void collapseToEnd();
    void extend(Node* node, int offset);
    void selectAllChildren(Node* node);
    bool containsNode(Node* node, bool allowPartial = false) const;

    // Fire pending selectionchange. Called by Document after mutation passes
    // so multiple updates within one frame collapse to a single event.
    void flushPendingChange();
    void schedulePendingChange() { pendingChange_ = true; }
    bool hasPendingChange() const { return pendingChange_; }

private:
    Document* document_;
    Range     range_;      // single live range, always attached to `document_`
    bool      hasRange_ = false;
    Direction direction_ = None;
    bool      pendingChange_ = false;
};

} // namespace bro::dom
