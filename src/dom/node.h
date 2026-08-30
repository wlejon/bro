#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace bro::dom {

enum class NodeType : uint8_t {
    Element = 1,
    Text = 3,
    Comment = 8,
    Document = 9,
    DocumentFragment = 11
};

class Element;   // forward
class Document;  // forward

class Node {
public:
    virtual ~Node() = default;
    virtual NodeType nodeType() const = 0;
    virtual std::string nodeName() const = 0;

    // Owner document. Every node allocated through Document::allocateNode gets
    // this set, so text and comment nodes can be lifetime-checked (NodeHandle)
    // and adopted across documents exactly like elements can.
    void setDocument(Document* doc) { document_ = doc; }
    Document* document() const { return document_; }

    Node* parentNode() const { return parent_; }
    void setParent(Node* p) { parent_ = p; }

    std::vector<Node*>& childNodes() { return children_; }
    const std::vector<Node*>& childNodes() const { return children_; }

    // Tree mutation. Each of these invalidates layout for the parent whose
    // child list moved (see notifyChildListChanged in element.cpp), so a node
    // inserted from C++ enters layout and renders — callers do NOT need to
    // follow up with markStructureDirty(). Safe to call from host code.
    //
    // They are the tree primitives only. They do not run script: custom-element
    // connectedCallback and MutationObserver delivery need a JS realm and stay
    // in the JS bindings, as does cross-document adoption (a pre-insertion step
    // that has to run before the tree changes). Host C++ inserting nodes it
    // created in this same document needs none of the three.
    void appendChild(Node* child);
    void removeChild(Node* child);
    void insertBefore(Node* newChild, Node* refChild);

    // Give up every child without destroying any of them: each becomes a
    // parentless root, alive and re-insertable.
    //
    // **What a doomed element does before it is freed.** Document::freeNode
    // destroys the whole subtree it is given, which is right for remove() — the
    // caller said it was finished with it — and wrong for the JS wrapper's
    // finalizer, which runs precisely when nothing in JS points at *this*
    // element any more. Something may still point at a child: the case that
    // found this is a card holding the <video> that decodes its preview across
    // a rebuild, where freeing the detached card destroyed the element the
    // holder was about to put back, leaving it with a wrapper whose node was
    // gone and a `cannot read property of undefined` two frames later.
    //
    // Detaching first is safe exactly because of when it happens. Nothing can
    // reach this element to notice that its children left; anything that reaches
    // a child through a reference of its own still has that child whole, with
    // its own subtree under it. The children become detached roots and are
    // reclaimed on their own terms — see DomBindings::sweepOrphanedWrappers.
    //
    // No notice is fired and none is owed: this node is leaving, it is in no
    // tree, and there is nothing to lay out.
    void releaseChildren();

    uint32_t nodeId() const { return id_; }

protected:
    Node* parent_ = nullptr;
    Document* document_ = nullptr;
    std::vector<Node*> children_;
    uint32_t id_ = nextId();
    static uint32_t nextId() { static uint32_t counter = 0; return ++counter; }
};

} // namespace bro::dom
