#include "dom/selection.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"

namespace bro::dom {

Selection::Selection(Document* doc) : document_(doc), range_(std::make_shared<Range>()) {
    range_->setDocument(doc);
}

Selection::~Selection() = default;

void Selection::detachShared() {
    if (range_.use_count() <= 1) return;
    auto fresh = std::make_shared<Range>();
    fresh->setDocument(document_);
    range_ = std::move(fresh);
}

Node* Selection::anchorNode() const {
    if (!hasRange_) return nullptr;
    return direction_ == Backward ? range_->endContainer() : range_->startContainer();
}

int Selection::anchorOffset() const {
    if (!hasRange_) return 0;
    return direction_ == Backward ? range_->endOffset() : range_->startOffset();
}

Node* Selection::focusNode() const {
    if (!hasRange_) return nullptr;
    return direction_ == Backward ? range_->startContainer() : range_->endContainer();
}

int Selection::focusOffset() const {
    if (!hasRange_) return 0;
    return direction_ == Backward ? range_->startOffset() : range_->endOffset();
}

const Range* Selection::getRangeAt(int index) const {
    if (index != 0 || !hasRange_) return nullptr;
    return range_.get();
}

Range* Selection::getRangeAt(int index) {
    if (index != 0 || !hasRange_) return nullptr;
    return range_.get();
}

std::shared_ptr<Range> Selection::sharedRangeAt(int index) {
    if (index != 0 || !hasRange_) return nullptr;
    return range_;
}

std::string Selection::type() const {
    if (!hasRange_) return "None";
    return range_->collapsed() ? "Caret" : "Range";
}

std::string Selection::toString() const {
    return hasRange_ ? range_->toString() : std::string{};
}

void Selection::setRange(Node* s, int sOff, Node* e, int eOff, Direction dir) {
    detachShared();
    if (!s || !e) {
        hasRange_ = false;
        direction_ = None;
    } else {
        range_->setStart(s, sOff);
        range_->setEnd(e, eOff);
        hasRange_ = true;
        direction_ = dir;
    }
    if (document_) document_->fireSelectionChange();
}

void Selection::addRange(const Range& r) {
    if (!r.startContainer() || !r.endContainer()) return;
    if (&r != range_.get()) {
        detachShared();
        range_->setStart(r.startContainer(), r.startOffset());
        range_->setEnd(r.endContainer(), r.endOffset());
    }
    hasRange_ = true;
    direction_ = Forward;
    if (document_) document_->fireSelectionChange();
}

void Selection::addSharedRange(std::shared_ptr<Range> r) {
    if (!r || !r->startContainer() || !r->endContainer()) return;
    // A range from another document cannot be this selection's range; take
    // a copy of its boundaries' shape instead of aliasing it.
    if (r->document() != document_) { addRange(*r); return; }
    range_ = std::move(r);
    hasRange_ = true;
    direction_ = Forward;
    if (document_) document_->fireSelectionChange();
}

void Selection::removeRange(const Range& r) {
    if (!hasRange_) return;
    if (&r == range_.get() ||
        (r.startContainer() == range_->startContainer() &&
         r.endContainer()   == range_->endContainer()   &&
         r.startOffset()    == range_->startOffset()    &&
         r.endOffset()      == range_->endOffset())) {
        removeAllRanges();
    }
}

void Selection::removeAllRanges() {
    if (!hasRange_) return;
    detachShared();
    hasRange_ = false;
    direction_ = None;
    if (document_) document_->fireSelectionChange();
}

void Selection::collapse(Node* node, int offset) {
    if (!node) { removeAllRanges(); return; }
    detachShared();
    range_->setStart(node, offset);
    range_->setEnd(node, offset);
    hasRange_ = true;
    direction_ = Forward;
    if (document_) document_->fireSelectionChange();
}

void Selection::collapseToStart() {
    if (!hasRange_) return;
    Node* n = range_->startContainer();
    int off = range_->startOffset();
    detachShared();
    range_->setStart(n, off);
    range_->setEnd(n, off);
    direction_ = Forward;
    if (document_) document_->fireSelectionChange();
}

void Selection::collapseToEnd() {
    if (!hasRange_) return;
    Node* n = range_->endContainer();
    int off = range_->endOffset();
    detachShared();
    range_->setStart(n, off);
    range_->setEnd(n, off);
    direction_ = Forward;
    if (document_) document_->fireSelectionChange();
}

// Extend the focus endpoint of the selection to (node, offset). If the focus
// moves backward past the anchor, swap direction.
void Selection::extend(Node* node, int offset) {
    if (!node) return;
    if (!hasRange_) { collapse(node, offset); return; }
    Node* anchorN = anchorNode();
    int anchorO = anchorOffset();
    detachShared();
    // Build a fresh range with anchor as one endpoint and the new focus as
    // the other; Range::setStart/setEnd + normalize() handles ordering.
    range_->setStart(anchorN, anchorO);
    range_->setEnd(anchorN, anchorO);
    // Determine which endpoint to move.
    // Range was collapsed to the anchor; now place the other endpoint.
    // If new point is before anchor → start=new, end=anchor (direction=Backward).
    // Else                         → start=anchor, end=new   (direction=Forward).
    // Use Range's own comparePoint via a throwaway check.
    Range probe;
    probe.setStart(anchorN, anchorO);
    probe.setEnd(anchorN, anchorO);
    probe.setEnd(node, offset);
    if (probe.startContainer() == node && probe.startOffset() == offset &&
        !(anchorN == node && anchorO == offset)) {
        // new point normalized to start -> it's before anchor
        range_->setStart(node, offset);
        range_->setEnd(anchorN, anchorO);
        direction_ = Backward;
    } else {
        range_->setStart(anchorN, anchorO);
        range_->setEnd(node, offset);
        direction_ = Forward;
    }
    if (document_) document_->fireSelectionChange();
}

void Selection::selectAllChildren(Node* node) {
    if (!node) return;
    detachShared();
    range_->selectNodeContents(node);
    hasRange_ = true;
    direction_ = Forward;
    if (document_) document_->fireSelectionChange();
}

bool Selection::containsNode(Node* node, bool allowPartial) const {
    if (!hasRange_ || !node) return false;
    if (allowPartial) return range_->intersectsNode(node);
    // Fully contained: start of node >= range start AND end of node <= range end.
    Node* parent = node->parentNode();
    if (!parent) return false;
    int idx = -1;
    const auto& kids = parent->childNodes();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i] == node) { idx = static_cast<int>(i); break; }
    if (idx < 0) return false;
    return range_->comparePoint(parent, idx) == 0 &&
           range_->comparePoint(parent, idx + 1) == 0;
}

void Selection::flushPendingChange() {
    // Retained for callers that batch mutations and want to coalesce events.
    // Synchronous dispatch from mutators makes this effectively a no-op
    // unless schedulePendingChange() was used instead.
    if (!pendingChange_) return;
    pendingChange_ = false;
    if (document_) document_->fireSelectionChange();
}

} // namespace bro::dom
