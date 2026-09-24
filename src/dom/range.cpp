#include "dom/range.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"
#include "dom/document_fragment.h"
#include <algorithm>

namespace bro::dom {

// ---------------------------------------------------------------------------
// Lifecycle + basic state
// ---------------------------------------------------------------------------

Range::Range() = default;

Range::~Range() {
    if (document_) document_->unregisterRange(this);
}

void Range::setDocument(Document* doc) {
    if (document_ == doc) return;
    if (document_) document_->unregisterRange(this);
    document_ = doc;
    if (document_) document_->registerRange(this);
}

bool Range::collapsed() const {
    return startContainer_ == endContainer_ && startOffset_ == endOffset_;
}

// ---------------------------------------------------------------------------
// Tree helpers
// ---------------------------------------------------------------------------

namespace {

// Index of `child` within `parent`'s children, or -1 if not found.
int indexOf(Node* parent, Node* child) {
    if (!parent || !child) return -1;
    const auto& kids = parent->childNodes();
    for (size_t i = 0; i < kids.size(); ++i)
        if (kids[i] == child) return static_cast<int>(i);
    return -1;
}

int childCountOrLen(Node* node) {
    if (!node) return 0;
    if (node->nodeType() == NodeType::Text) {
        return static_cast<int>(static_cast<TextNode*>(node)->length());
    }
    if (node->nodeType() == NodeType::Comment) {
        return static_cast<int>(static_cast<CommentNode*>(node)->data().size());
    }
    return static_cast<int>(node->childNodes().size());
}

// Ancestor chain root→node (inclusive).
std::vector<Node*> ancestorsInclusive(Node* node) {
    std::vector<Node*> out;
    for (Node* n = node; n; n = n->parentNode()) out.push_back(n);
    std::reverse(out.begin(), out.end());
    return out;
}

// True when `ancestor` is `node` or one of its ancestors.
bool isAncestorOf(Node* ancestor, Node* node) {
    for (Node* n = node; n; n = n->parentNode())
        if (n == ancestor) return true;
    return false;
}

// Return -1 / 0 / +1 for (a,aOff) vs (b,bOff) in tree order. Returns 0 if
// either side is null or the nodes aren't in the same tree.
int comparePositions(Node* a, int aOff, Node* b, int bOff) {
    if (!a || !b) return 0;
    if (a == b) {
        if (aOff < bOff) return -1;
        if (aOff > bOff) return 1;
        return 0;
    }
    auto pa = ancestorsInclusive(a);
    auto pb = ancestorsInclusive(b);
    if (pa.front() != pb.front()) return 0; // disjoint trees

    size_t i = 0;
    while (i < pa.size() && i < pb.size() && pa[i] == pb[i]) ++i;
    if (i == pa.size()) {
        // a is an ancestor of b: compare aOff (child index in a) against the
        // position of b's ancestor-within-a.
        Node* bBranch = pb[i];
        int idx = indexOf(a, bBranch);
        if (aOff <= idx) return -1;
        return 1;
    }
    if (i == pb.size()) {
        Node* aBranch = pa[i];
        int idx = indexOf(b, aBranch);
        if (idx < bOff) return -1;
        return 1;
    }
    // Diverged at common parent pa[i-1]; compare child indices.
    Node* common = pa[i-1];
    int aIdx = indexOf(common, pa[i]);
    int bIdx = indexOf(common, pb[i]);
    if (aIdx < bIdx) return -1;
    if (aIdx > bIdx) return 1;
    return 0;
}

// Deepest common ancestor of two nodes.
Node* commonAncestor(Node* a, Node* b) {
    if (!a || !b) return nullptr;
    auto pa = ancestorsInclusive(a);
    auto pb = ancestorsInclusive(b);
    if (pa.empty() || pb.empty() || pa.front() != pb.front()) return nullptr;
    Node* c = nullptr;
    size_t n = std::min(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] == pb[i]) c = pa[i];
        else break;
    }
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// Boundary setters
// ---------------------------------------------------------------------------

void Range::setStart(Node* node, int offset) {
    if (!node) return;
    startContainer_ = node;
    startOffset_ = std::max(0, std::min(offset, childCountOrLen(node)));
    normalize();
}

void Range::setEnd(Node* node, int offset) {
    if (!node) return;
    endContainer_ = node;
    endOffset_ = std::max(0, std::min(offset, childCountOrLen(node)));
    normalize();
}

void Range::setStartBefore(Node* node) {
    if (!node || !node->parentNode()) return;
    int idx = indexOf(node->parentNode(), node);
    if (idx >= 0) setStart(node->parentNode(), idx);
}

void Range::setStartAfter(Node* node) {
    if (!node || !node->parentNode()) return;
    int idx = indexOf(node->parentNode(), node);
    if (idx >= 0) setStart(node->parentNode(), idx + 1);
}

void Range::setEndBefore(Node* node) {
    if (!node || !node->parentNode()) return;
    int idx = indexOf(node->parentNode(), node);
    if (idx >= 0) setEnd(node->parentNode(), idx);
}

void Range::setEndAfter(Node* node) {
    if (!node || !node->parentNode()) return;
    int idx = indexOf(node->parentNode(), node);
    if (idx >= 0) setEnd(node->parentNode(), idx + 1);
}

void Range::collapse(bool toStart) {
    if (toStart) {
        endContainer_ = startContainer_;
        endOffset_ = startOffset_;
    } else {
        startContainer_ = endContainer_;
        startOffset_ = endOffset_;
    }
}

void Range::selectNode(Node* node) {
    if (!node || !node->parentNode()) return;
    int idx = indexOf(node->parentNode(), node);
    if (idx < 0) return;
    startContainer_ = endContainer_ = node->parentNode();
    startOffset_ = idx;
    endOffset_ = idx + 1;
}

void Range::selectNodeContents(Node* node) {
    if (!node) return;
    startContainer_ = endContainer_ = node;
    startOffset_ = 0;
    endOffset_ = childCountOrLen(node);
}

// Keep start before or equal to end. If end precedes start, collapse both to
// the start.
void Range::normalize() {
    if (!startContainer_ || !endContainer_) {
        if (startContainer_) { endContainer_ = startContainer_; endOffset_ = startOffset_; }
        else if (endContainer_) { startContainer_ = endContainer_; startOffset_ = endOffset_; }
        return;
    }
    if (comparePositions(startContainer_, startOffset_,
                         endContainer_, endOffset_) > 0) {
        endContainer_ = startContainer_;
        endOffset_ = startOffset_;
    }
}

// ---------------------------------------------------------------------------
// Comparisons
// ---------------------------------------------------------------------------

Node* Range::commonAncestorContainer() const {
    return commonAncestor(startContainer_, endContainer_);
}

int Range::comparePoint(Node* node, int offset) const {
    if (comparePositions(node, offset, startContainer_, startOffset_) < 0) return -1;
    if (comparePositions(node, offset, endContainer_, endOffset_) > 0)     return  1;
    return 0;
}

bool Range::isPointInRange(Node* node, int offset) const {
    return comparePoint(node, offset) == 0;
}

bool Range::intersectsNode(Node* node) const {
    if (!node || !startContainer_ || !endContainer_) return false;
    Node* parent = node->parentNode();
    if (!parent) return node == commonAncestorContainer();
    int idx = indexOf(parent, node);
    if (idx < 0) return false;
    return comparePositions(parent, idx, endContainer_, endOffset_) < 0 &&
           comparePositions(parent, idx + 1, startContainer_, startOffset_) > 0;
}

// ---------------------------------------------------------------------------
// toString
// ---------------------------------------------------------------------------

namespace {

void collectRangeText(Node* startC, int startOff, Node* endC, int endOff,
                      Node* node, bool& started, bool& done, std::string& out) {
    if (done) return;
    if (!started && node == startC && node->nodeType() != NodeType::Element) {
        started = true;
        if (node->nodeType() == NodeType::Text) {
            auto* tn = static_cast<TextNode*>(node);
            int from = std::min(startOff, static_cast<int>(tn->length()));
            if (node == endC) {
                int to = std::min(endOff, static_cast<int>(tn->length()));
                out += tn->data().substr(from, std::max(0, to - from));
                done = true;
            } else {
                out += tn->data().substr(from);
            }
        }
        return;
    }

    if (node->nodeType() == NodeType::Element) {
        // Handle "startContainer is this element, offset = N → start at child N".
        size_t first = 0;
        if (!started && node == startC) {
            started = true;
            first = static_cast<size_t>(std::max(0, startOff));
        }
        auto& kids = node->childNodes();
        for (size_t i = first; i < kids.size() && !done; ++i) {
            if (!started && node == endC && static_cast<int>(i) >= endOff) {
                done = true; break;
            }
            if (started && node == endC && static_cast<int>(i) >= endOff) {
                done = true; break;
            }
            collectRangeText(startC, startOff, endC, endOff, kids[i], started, done, out);
        }
        if (!done && node == endC) done = true;
        return;
    }

    if (started && node->nodeType() == NodeType::Text) {
        auto* tn = static_cast<TextNode*>(node);
        if (node == endC) {
            int to = std::min(endOff, static_cast<int>(tn->length()));
            out += tn->data().substr(0, to);
            done = true;
        } else {
            out += tn->data();
        }
    }
}

} // namespace

std::string Range::toString() const {
    if (!startContainer_ || !endContainer_) return {};
    Node* root = commonAncestorContainer();
    if (!root) return {};
    std::string out;
    bool started = false, done = false;
    collectRangeText(startContainer_, startOffset_, endContainer_, endOffset_,
                     root, started, done, out);
    return out;
}

// ---------------------------------------------------------------------------
// Content manipulation
//
// The DOM standard's "clone the contents", "extract" and "delete" algorithms
// (https://dom.spec.whatwg.org/#concept-range-clone), written the way the
// spec writes them: split the common ancestor's children into the first
// partially contained child, the fully contained ones, and the last partially
// contained child, and recurse into the partial ones through a sub-range. A
// partially selected element is therefore cloned SHALLOW into the fragment
// with just its selected descendants inside — `ect any <em>live</em>` for a
// range from inside a text node into an <em> — which the old
// "collect-the-whole-nodes" walk could not express at all.
// ---------------------------------------------------------------------------

namespace {

bool isCharData(const Node* n) {
    return n && (n->nodeType() == NodeType::Text || n->nodeType() == NodeType::Comment);
}

const std::string& charData(Node* n) {
    if (n->nodeType() == NodeType::Text) return static_cast<TextNode*>(n)->data();
    return static_cast<CommentNode*>(n)->data();
}

// "Replace data": through the node's own mutator, so live ranges (this one
// included) and mutation observers see it the way they see a script's edit.
void deleteCharData(Node* n, int offset, int count) {
    if (count <= 0) return;
    if (n->nodeType() == NodeType::Text)
        static_cast<TextNode*>(n)->deleteData(static_cast<size_t>(offset),
                                              static_cast<size_t>(count));
    else if (n->nodeType() == NodeType::Comment)
        static_cast<CommentNode*>(n)->deleteData(static_cast<size_t>(offset),
                                                 static_cast<size_t>(count));
}

// A same-type node holding data[from, to).
Node* cloneCharSubstring(Document* doc, Node* n, int from, int to) {
    const std::string& data = charData(n);
    int lo = std::clamp(from, 0, static_cast<int>(data.size()));
    int hi = std::clamp(to, lo, static_cast<int>(data.size()));
    std::string piece = data.substr(lo, hi - lo);
    if (n->nodeType() == NodeType::Text) return doc->createTextNode(piece);
    return doc->createComment(piece);
}

// "Contained": (node, 0) is after the start and (node, length) before the end.
bool isContained(Node* n, Node* sC, int sO, Node* eC, int eO) {
    return comparePositions(n, 0, sC, sO) > 0 &&
           comparePositions(n, childCountOrLen(n), eC, eO) < 0;
}

// Clone (extract == false) or extract the contents of [(sC,sO), (eC,eO)] into
// `out` — the fragment, or the shallow clone of a partially contained element
// one level up. Offsets are UTF-8 byte offsets for character data.
void contentsInto(Document* doc, Node* out, Node* sC, int sO, Node* eC, int eO,
                  bool extract) {
    if (sC == eC && sO == eO) return;

    if (sC == eC && isCharData(sC)) {
        out->appendChild(cloneCharSubstring(doc, sC, sO, eO));
        if (extract) deleteCharData(sC, sO, eO - sO);
        return;
    }

    Node* common = sC;
    while (common && !isAncestorOf(common, eC)) common = common->parentNode();
    if (!common) return;

    Node* firstPartial = nullptr;
    if (!isAncestorOf(sC, eC)) {
        for (Node* c : common->childNodes())
            if (isAncestorOf(c, sC)) { firstPartial = c; break; }
    }
    Node* lastPartial = nullptr;
    if (!isAncestorOf(eC, sC)) {
        const auto& kids = common->childNodes();
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            if (isAncestorOf(*it, eC)) { lastPartial = *it; break; }
    }
    std::vector<Node*> contained;
    for (Node* c : common->childNodes())
        if (isContained(c, sC, sO, eC, eO)) contained.push_back(c);

    if (firstPartial) {
        if (isCharData(firstPartial)) {
            // firstPartial is the start node itself.
            int len = childCountOrLen(sC);
            out->appendChild(cloneCharSubstring(doc, sC, sO, len));
            if (extract) deleteCharData(sC, sO, len - sO);
        } else {
            Node* clone = doc->cloneNode(firstPartial, /*deep=*/false);
            if (clone) {
                out->appendChild(clone);
                contentsInto(doc, clone, sC, sO, firstPartial,
                             childCountOrLen(firstPartial), extract);
            }
        }
    }

    for (Node* c : contained) {
        if (extract) {
            out->appendChild(c);  // detaches it from the source tree
        } else if (Node* clone = doc->cloneNode(c, /*deep=*/true)) {
            out->appendChild(clone);
        }
    }

    if (lastPartial) {
        if (isCharData(lastPartial)) {
            out->appendChild(cloneCharSubstring(doc, eC, 0, eO));
            if (extract) deleteCharData(eC, 0, eO);
        } else {
            Node* clone = doc->cloneNode(lastPartial, /*deep=*/false);
            if (clone) {
                out->appendChild(clone);
                contentsInto(doc, clone, lastPartial, 0, eC, eO, extract);
            }
        }
    }
}

// Where the range collapses after extract/delete: the start, when the start
// node contains the end; otherwise just after the start's topmost ancestor
// that does not contain the end.
void collapsePointAfterRemoval(Node* sC, int sO, Node* eC, Node*& node, int& off) {
    if (isAncestorOf(sC, eC)) { node = sC; off = sO; return; }
    Node* ref = sC;
    while (ref->parentNode() && !isAncestorOf(ref->parentNode(), eC))
        ref = ref->parentNode();
    node = ref->parentNode();
    off = indexOf(node, ref) + 1;
}

// Split `tn` at `offset` (the Text.splitText algorithm): the tail becomes a new
// sibling right after it, and live-range endpoints past the split move with
// the characters they sat between.
TextNode* splitTextNode(Document* doc, TextNode* tn, int offset) {
    int len = static_cast<int>(tn->length());
    offset = std::clamp(offset, 0, len);
    auto* tail = doc->createTextNode(tn->data().substr(offset));
    if (Node* parent = tn->parentNode()) {
        const auto& kids = parent->childNodes();
        int idx = indexOf(parent, tn);
        if (idx + 1 < static_cast<int>(kids.size()))
            parent->insertBefore(tail, kids[idx + 1]);
        else
            parent->appendChild(tail);
    }
    doc->notifyTextSplit(tn, offset, tail);
    tn->deleteData(static_cast<size_t>(offset), static_cast<size_t>(len - offset));
    return tail;
}

} // namespace

void Range::deleteContents() {
    if (!document_ || !startContainer_ || !endContainer_ || collapsed()) return;
    // Extract into a scratch fragment: the source-tree effect of "delete" is
    // exactly extract's (trim the partial character data, detach the contained
    // nodes, empty the partial elements' selected descendants). Then the
    // scratch fragment and what it holds are freed, except the nodes a script
    // may still hold (as it may after removeChild): those stay, detached.
    Node* scratch = extractContents();
    if (scratch) document_->freeUnlessRetained(scratch);
}

Node* Range::cloneContents() const {
    if (!document_) return nullptr;
    auto* frag = document_->createElement("#DOCUMENT-FRAGMENT");
    if (!startContainer_ || !endContainer_ || collapsed()) return frag;
    contentsInto(document_, frag, startContainer_, startOffset_,
                 endContainer_, endOffset_, /*extract=*/false);
    return frag;
}

Node* Range::extractContents() {
    if (!document_) return nullptr;
    auto* frag = document_->createElement("#DOCUMENT-FRAGMENT");
    if (!startContainer_ || !endContainer_ || collapsed()) return frag;

    Node* sC = startContainer_;
    int sO = startOffset_;
    Node* eC = endContainer_;
    int eO = endOffset_;
    Node* newNode = sC;
    int newOff = sO;
    if (!(sC == eC && isCharData(sC)))
        collapsePointAfterRemoval(sC, sO, eC, newNode, newOff);

    contentsInto(document_, frag, sC, sO, eC, eO, /*extract=*/true);

    startContainer_ = endContainer_ = newNode;
    startOffset_ = endOffset_ = std::clamp(newOff, 0, childCountOrLen(newNode));
    return frag;
}

Range::Error Range::insertNode(Node* node) {
    if (!node || !startContainer_ || !document_) return Error::None;
    Node* start = startContainer_;
    if (start->nodeType() == NodeType::Comment ||
        (start->nodeType() == NodeType::Text && !start->parentNode()) ||
        start == node)
        return Error::HierarchyRequest;

    Node* reference = nullptr;
    if (start->nodeType() == NodeType::Text) {
        reference = start;
    } else {
        const auto& kids = start->childNodes();
        if (startOffset_ < static_cast<int>(kids.size())) reference = kids[startOffset_];
    }
    Node* parent = reference ? reference->parentNode() : start;
    // Pre-insertion validity: the node may not be an inclusive ancestor of
    // the parent it is going into.
    if (isAncestorOf(node, parent)) return Error::HierarchyRequest;

    if (start->nodeType() == NodeType::Text)
        reference = splitTextNode(document_, static_cast<TextNode*>(start), startOffset_);
    if (node == reference) {
        int idx = indexOf(parent, reference);
        const auto& kids = parent->childNodes();
        reference = (idx >= 0 && idx + 1 < static_cast<int>(kids.size())) ? kids[idx + 1]
                                                                          : nullptr;
    }
    if (node->parentNode()) node->parentNode()->removeChild(node);

    int newOffset = reference ? indexOf(parent, reference)
                              : static_cast<int>(parent->childNodes().size());
    const bool isFragment = node->nodeType() == NodeType::DocumentFragment ||
        (node->nodeType() == NodeType::Element &&
         static_cast<Element*>(node)->tagName() == "#DOCUMENT-FRAGMENT");
    newOffset += isFragment ? static_cast<int>(node->childNodes().size()) : 1;
    const bool wasCollapsed = collapsed();

    if (isFragment) {
        auto kids = node->childNodes();
        for (Node* k : kids) parent->insertBefore(k, reference);
    } else {
        parent->insertBefore(node, reference);
    }
    if (wasCollapsed) {
        endContainer_ = parent;
        endOffset_ = newOffset;
    }
    return Error::None;
}

Range::Error Range::surroundContents(Element* newParent) {
    if (!newParent || !document_ || !startContainer_ || !endContainer_) return Error::None;
    // A non-Text node partially contained in the range cannot be wrapped: the
    // result would have to split it.
    Node* common = commonAncestorContainer();
    for (Node* n = startContainer_; n && n != common; n = n->parentNode())
        if (n->nodeType() != NodeType::Text && !isAncestorOf(n, endContainer_))
            return Error::InvalidState;
    for (Node* n = endContainer_; n && n != common; n = n->parentNode())
        if (n->nodeType() != NodeType::Text && !isAncestorOf(n, startContainer_))
            return Error::InvalidState;
    if (newParent->tagName() == "#DOCUMENT-FRAGMENT") return Error::InvalidNodeType;

    Node* fragment = extractContents();
    auto oldKids = newParent->childNodes();
    for (Node* k : oldKids) newParent->removeChild(k);
    if (Error e = insertNode(newParent); e != Error::None) return e;
    if (fragment) {
        auto kids = fragment->childNodes();
        for (Node* k : kids) newParent->appendChild(k);
        document_->freeUnlessRetained(fragment);   // the emptied scratch
    }
    selectNode(newParent);
    return Error::None;
}

Node* Range::createContextualFragment(const std::string& html) {
    if (!document_ || !startContainer_) return nullptr;
    auto* frag = document_->createElement("#DOCUMENT-FRAGMENT");
    document_->parseInnerHTML(frag, html);
    return frag;
}

Range* Range::cloneRange() const {
    auto* r = new Range();
    r->startContainer_ = startContainer_;
    r->startOffset_ = startOffset_;
    r->endContainer_ = endContainer_;
    r->endOffset_ = endOffset_;
    r->setDocument(document_);
    return r;
}

// ---------------------------------------------------------------------------
// Live-mutation updates
// ---------------------------------------------------------------------------

namespace {

bool inSubtree(Node* root, Node* n) {
    for (Node* p = n; p; p = p->parentNode())
        if (p == root) return true;
    return false;
}

} // namespace

void Range::onNodeRemoved(Node* removed, Node* parent, int indexInParent) {
    auto adjust = [&](Node*& c, int& off) {
        if (!c) return;
        if (inSubtree(removed, c)) {
            c = parent;
            off = indexInParent;
            return;
        }
        // If endpoint is in the removed node's former parent and tracks a
        // child index after `removed`, shift left by one.
        if (c == parent && off > indexInParent) off--;
    };
    adjust(startContainer_, startOffset_);
    adjust(endContainer_, endOffset_);
}

void Range::onTextDataChanged(Node* node, int offset, int count, int newLen) {
    auto adjust = [&](Node* c, int& off) {
        if (c != node) return;
        int delta = newLen - count;
        if (off > offset + count) off += delta;
        else if (off > offset) off = offset + newLen;
    };
    adjust(startContainer_, startOffset_);
    adjust(endContainer_, endOffset_);
}

void Range::onTextSplit(Node* node, int offset, Node* tail) {
    auto adjust = [&](Node*& c, int& off) {
        if (c != node) return;
        if (off > offset) { c = tail; off -= offset; }
    };
    adjust(startContainer_, startOffset_);
    adjust(endContainer_, endOffset_);
}

// DOM "insert" step: a boundary point in `parent` AFTER the insertion index
// shifts right; one exactly AT it stays put, so it now sits before the
// inserted node (which is what keeps a collapsed caret ahead of text a script
// inserts at it — insertNode then moves the end explicitly).
void Range::onChildInserted(Node* parent, int index) {
    auto adjust = [&](Node* c, int& off) {
        if (c == parent && off > index) off++;
    };
    adjust(startContainer_, startOffset_);
    adjust(endContainer_, endOffset_);
}

void Range::onNodeDestroyed(Node* destroyed) {
    if (startContainer_ == destroyed) {
        startContainer_ = nullptr;
        startOffset_ = 0;
    }
    if (endContainer_ == destroyed) {
        endContainer_ = nullptr;
        endOffset_ = 0;
    }
}

} // namespace bro::dom
