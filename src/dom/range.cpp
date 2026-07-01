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
// ---------------------------------------------------------------------------

namespace {

// Split a text node at `offset` and return the tail so the caller can splice
// ranges. The tail is inserted immediately after the original.
TextNode* splitAt(TextNode* tn, int offset) {
    if (!tn || offset <= 0 || offset >= static_cast<int>(tn->length())) return nullptr;
    Node* parent = tn->parentNode();
    if (!parent) return nullptr;
    auto* doc = [&]() -> Document* {
        auto* p = parent;
        while (p && p->nodeType() != NodeType::Document) {
            if (p->nodeType() == NodeType::Element)
                return static_cast<Element*>(p)->document();
            p = p->parentNode();
        }
        return nullptr;
    }();
    if (!doc) return nullptr;
    auto* tail = doc->createTextNode(tn->data().substr(offset));
    tn->setData(tn->data().substr(0, offset));
    // Insert after tn
    int idx = indexOf(parent, tn);
    if (idx < 0) return nullptr;
    auto& kids = parent->childNodes();
    if (idx + 1 >= static_cast<int>(kids.size())) {
        parent->appendChild(tail);
    } else {
        parent->insertBefore(tail, kids[idx + 1]);
    }
    return tail;
}

// Collect the top-level nodes "fully contained" within [start,end]. Partially
// contained text endpoints are split so the region between them is a clean
// run of whole nodes.
std::vector<Node*> contentsInRange(Range& range) {
    std::vector<Node*> result;
    Node* startC = range.startContainer();
    Node* endC = range.endContainer();
    int startOff = range.startOffset();
    int endOff = range.endOffset();
    if (!startC || !endC) return result;

    // Single text container: split, capture middle.
    if (startC == endC && startC->nodeType() == NodeType::Text) {
        auto* tn = static_cast<TextNode*>(startC);
        if (startOff > 0) {
            auto* after = splitAt(tn, startOff);
            if (after) {
                // Adjust endOff into the new node
                int newEnd = endOff - startOff;
                if (newEnd > 0 && newEnd < static_cast<int>(after->length())) {
                    splitAt(after, newEnd);
                }
                result.push_back(after);
                return result;
            }
        } else {
            if (endOff < static_cast<int>(tn->length())) {
                splitAt(tn, endOff);
            }
            result.push_back(tn);
            return result;
        }
    }

    // If endpoints land in text nodes, split so the boundary becomes between
    // nodes rather than inside them.
    if (startC->nodeType() == NodeType::Text && startOff > 0 &&
        startOff < static_cast<int>(static_cast<TextNode*>(startC)->length())) {
        auto* after = splitAt(static_cast<TextNode*>(startC), startOff);
        if (after) { startC = after; startOff = 0; }
    }
    if (endC->nodeType() == NodeType::Text && endOff > 0 &&
        endOff < static_cast<int>(static_cast<TextNode*>(endC)->length())) {
        splitAt(static_cast<TextNode*>(endC), endOff);
    }

    Node* common = commonAncestor(startC, endC);
    if (!common) return result;

    // Walk common's descendants in tree order; collect the topmost nodes
    // fully inside the range.
    std::function<void(Node*)> walk = [&](Node* n) {
        if (!n) return;
        // Determine if n is fully contained.
        Node* parent = n->parentNode();
        if (parent) {
            int idx = indexOf(parent, n);
            bool afterS = comparePositions(parent, idx, startC, startOff) >= 0;
            bool beforeE = comparePositions(parent, idx + 1, endC, endOff) <= 0;
            if (afterS && beforeE) {
                result.push_back(n);
                return;
            }
        }
        for (auto* child : n->childNodes()) walk(child);
    };

    for (auto* child : common->childNodes()) walk(child);
    return result;
}

} // namespace

void Range::deleteContents() {
    if (collapsed()) return;
    auto nodes = contentsInRange(*this);
    Node* start = startContainer_;
    int startOff = startOffset_;
    Document* doc = document_;
    for (auto* n : nodes) {
        if (!n) continue;
        Node* p = n->parentNode();
        if (p) p->removeChild(n);
        if (doc) doc->freeNode(n);
    }
    // Collapse to the start position (end endpoint may now be invalid).
    startContainer_ = endContainer_ = start;
    startOffset_ = endOffset_ = startOff;
}

Node* Range::cloneContents() const {
    if (!document_) return nullptr;
    auto* frag = document_->createElement("#DOCUMENT-FRAGMENT");
    if (collapsed()) return frag;

    // A cheap approximation: walk the range tree-in-order, clone each node.
    // Sufficient for the common "single ancestor, mix of text + elements" case.
    Range tmp;
    tmp.startContainer_ = startContainer_;
    tmp.startOffset_ = startOffset_;
    tmp.endContainer_ = endContainer_;
    tmp.endOffset_ = endOffset_;
    auto nodes = contentsInRange(tmp);

    // contentsInRange may have split text nodes in the live tree. Since we're
    // cloning, undo nothing — the splits are benign.
    for (auto* n : nodes) {
        if (!n) continue;
        if (n->nodeType() == NodeType::Text) {
            auto* clone = document_->createTextNode(
                static_cast<TextNode*>(n)->data());
            frag->appendChild(clone);
        } else if (n->nodeType() == NodeType::Element) {
            // Clone via outerHTML round-trip — preserves nested structure.
            auto* src = static_cast<Element*>(n);
            auto* holder = document_->createElement("DIV");
            document_->parseInnerHTML(holder, src->outerHTML());
            auto kids = holder->childNodes();
            for (auto* k : kids) k->setParent(nullptr);
            holder->childNodes().clear();
            for (auto* k : kids) frag->appendChild(k);
            document_->freeNode(holder);
        } else if (n->nodeType() == NodeType::Comment) {
            auto* clone = document_->createComment(
                static_cast<CommentNode*>(n)->data());
            frag->appendChild(clone);
        }
    }
    return frag;
}

Node* Range::extractContents() {
    if (!document_) return nullptr;
    auto* frag = document_->createElement("#DOCUMENT-FRAGMENT");
    if (collapsed()) return frag;

    auto nodes = contentsInRange(*this);
    Node* start = startContainer_;
    int startOff = startOffset_;
    for (auto* n : nodes) {
        if (!n) continue;
        Node* p = n->parentNode();
        if (p) p->removeChild(n);
        frag->appendChild(n);
    }
    startContainer_ = endContainer_ = start;
    startOffset_ = endOffset_ = startOff;
    return frag;
}

void Range::insertNode(Node* node) {
    if (!node || !startContainer_) return;
    if (startContainer_->nodeType() == NodeType::Text) {
        auto* tn = static_cast<TextNode*>(startContainer_);
        auto* parent = tn->parentNode();
        if (!parent) return;
        if (startOffset_ == 0) {
            parent->insertBefore(node, tn);
        } else if (startOffset_ >= static_cast<int>(tn->length())) {
            int idx = indexOf(parent, tn);
            auto& kids = parent->childNodes();
            if (idx + 1 >= static_cast<int>(kids.size())) parent->appendChild(node);
            else parent->insertBefore(node, kids[idx + 1]);
        } else {
            auto* tail = splitAt(tn, startOffset_);
            if (tail) parent->insertBefore(node, tail);
            else parent->appendChild(node);
        }
        return;
    }
    // Element container: insert at the child index.
    auto& kids = startContainer_->childNodes();
    if (startOffset_ >= static_cast<int>(kids.size())) {
        startContainer_->appendChild(node);
    } else {
        startContainer_->insertBefore(node, kids[startOffset_]);
    }
}

void Range::surroundContents(Element* newParent) {
    if (!newParent) return;
    auto* extracted = extractContents();
    insertNode(newParent);
    if (extracted) {
        auto kids = extracted->childNodes();
        for (auto* k : kids) k->setParent(nullptr);
        extracted->childNodes().clear();
        for (auto* k : kids) newParent->appendChild(k);
        if (document_) document_->freeNode(extracted);
    }
    selectNode(newParent);
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

void Range::onChildInserted(Node* parent, int index) {
    auto adjust = [&](Node* c, int& off) {
        if (c == parent && off >= index) off++;
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
