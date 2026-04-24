#pragma once
#include "dom/node.h"
#include <string>

namespace bro::dom {

class Document;
class Element;

// DOM Range (https://dom.spec.whatwg.org/#interface-range).
//
// Endpoints are (container, offset). For Element containers offset is a child
// index [0, children.size()]; for Text/Comment containers offset is a UTF-8
// byte offset into the character data.
//
// Live Ranges register themselves with their owning Document so mutations can
// update or collapse endpoints. Freed via `delete` from the JS finalizer —
// caller must call setDocument(nullptr) before deleting to unregister.
class Range {
public:
    Range();
    ~Range();

    Range(const Range&) = delete;
    Range& operator=(const Range&) = delete;

    Node* startContainer() const { return startContainer_; }
    Node* endContainer()   const { return endContainer_; }
    int   startOffset()    const { return startOffset_; }
    int   endOffset()      const { return endOffset_; }
    bool  collapsed()      const;

    // Document registration for live-mutation updates.
    void setDocument(Document* doc);
    Document* document() const { return document_; }

    // Boundary setters. node==nullptr leaves the endpoint unchanged.
    void setStart(Node* node, int offset);
    void setEnd(Node* node, int offset);
    void setStartBefore(Node* node);
    void setStartAfter(Node* node);
    void setEndBefore(Node* node);
    void setEndAfter(Node* node);

    // collapse(true) → collapse to start; collapse(false) → collapse to end.
    void collapse(bool toStart);

    // Selection helpers.
    void selectNode(Node* node);
    void selectNodeContents(Node* node);

    // Deepest ancestor containing both endpoints.
    Node* commonAncestorContainer() const;

    // Tree-order comparison: returns -1 if (node,offset) precedes start, 0 if
    // inside [start,end], +1 if after end. Undefined if node isn't in the
    // same tree as the range.
    int comparePoint(Node* node, int offset) const;
    bool isPointInRange(Node* node, int offset) const;
    bool intersectsNode(Node* node) const;

    // Serialize the text content between endpoints.
    std::string toString() const;

    // Content manipulation. Caller owns lifetime of returned / inserted nodes
    // via Document::ownedNodes_.
    void deleteContents();
    Node* cloneContents() const;   // returns a DocumentFragment
    Node* extractContents();       // returns a DocumentFragment
    void insertNode(Node* node);
    void surroundContents(Element* newParent);

    // Parse `html` in the context of startContainer() and return a fragment.
    // Returns nullptr if the range has no document or container.
    Node* createContextualFragment(const std::string& html);

    // Produce an independent copy with the same endpoints.
    Range* cloneRange() const;

    // Mutation notifications. Called by Document when the tree changes.
    //
    // onNodeRemoved: `removed` and its subtree are being detached from `parent`
    // at `indexInParent`. If an endpoint is inside the removed subtree, it is
    // moved to (parent, indexInParent). Also fires when endpoints track the
    // index of following siblings.
    void onNodeRemoved(Node* removed, Node* parent, int indexInParent);
    // onTextDataChanged: TextNode `node` had `count` chars replaced at
    // `offset` with `newLen` chars. Offsets after the change shift.
    void onTextDataChanged(Node* node, int offset, int count, int newLen);
    // onTextSplit: TextNode `node` was split at `offset`; characters at
    // [offset, len) moved to `tail`. Endpoints in the tail region retarget.
    void onTextSplit(Node* node, int offset, Node* tail);
    // onChildInserted: a node was inserted at `index` under `parent`. Child-
    // index endpoints on `parent` that are >= index shift up.
    void onChildInserted(Node* parent, int index);
    // onNodeDestroyed: `destroyed` is about to be deallocated. Called by
    // Document::freeNode as a safety net — some mutation paths (setTextContent,
    // innerHTML replacement) skip onNodeRemoved, so endpoints can end up
    // pointing at memory that's being freed. Clears any endpoint still
    // referencing this specific node.
    void onNodeDestroyed(Node* destroyed);

private:
    Node* startContainer_ = nullptr;
    int   startOffset_    = 0;
    Node* endContainer_   = nullptr;
    int   endOffset_      = 0;
    Document* document_   = nullptr;

    void normalize();
};

} // namespace bro::dom
