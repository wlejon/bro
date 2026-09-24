// Node ownership: the factories every node in this document comes from, the
// clone and adopt algorithms that move nodes between owners, the deferred free
// that keeps a pointer valid while another thread may still be holding it, and
// the liveness answers (ownsNode / isNodeLive / resolveNode) plus the observer
// lists the layers above hang off those events.
//
// Split out of document.cpp, which had grown past the size this repo keeps its
// translation units to. Nothing here resolves style or touches the cascade; it
// is the storage half of a Document and reads as one.

#include "dom/document.h"
#include "dom/range.h"
#include "engine/css_transitions.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace bro::dom {

Element* Document::createElement(const std::string& tag) {
    auto* elem = allocateNode<Element>(tag);
    elem->setDocument(this);
    return elem;
}

TextNode* Document::createTextNode(const std::string& text) {
    return allocateNode<TextNode>(text);
}

CommentNode* Document::createComment(const std::string& data) {
    return allocateNode<CommentNode>(data);
}

DocumentFragment* Document::createDocumentFragment() {
    return allocateNode<DocumentFragment>();
}

Node* Document::cloneNode(Node* src, bool deep, bool preserveId) {
    if (!src) return nullptr;

    switch (src->nodeType()) {
        case NodeType::Text:
            return createTextNode(static_cast<TextNode*>(src)->data());
        case NodeType::Comment:
            return createComment(static_cast<CommentNode*>(src)->data());
        case NodeType::DocumentFragment: {
            Node* clone = nullptr;
            if (auto* el = dynamic_cast<Element*>(src)) {
                clone = createElement(el->tagName());
                if (auto* cel = dynamic_cast<Element*>(clone)) {
                    cel->setIsTemplateContent(el->isTemplateContent());
                }
            } else {
                clone = createDocumentFragment();
            }
            if (deep && clone) {
                for (auto* child : src->childNodes()) {
                    if (Node* childClone = cloneNode(child, true, preserveId))
                        clone->appendChild(childClone);
                }
            }
            return clone;
        }
        case NodeType::Element:
            break;
        default:
            return nullptr;
    }

    auto* srcEl = static_cast<Element*>(src);
    Element* clone = createElement(srcEl->tagName());
    if (!clone) return nullptr;
    if (srcEl->ns() == Element::Namespace::Other) clone->setNamespaceURI(srcEl->namespaceURI());
    else clone->setNs(srcEl->ns());
    clone->setQualifiedName(srcEl->qualifiedName());

    for (const auto& [name, val] : srcEl->attributes()) {
        if (!preserveId && name == "id") continue;
        clone->setAttribute(name, val);
    }
    // "style" never lives in attributes_ (see Element::setAttribute) — copy the
    // declaration block from StyleProxy, and only when the attribute is really
    // present, so an element with no style attribute doesn't grow one.
    if (srcEl->hasAttribute("style"))
        clone->setAttribute("style", srcEl->style().cssText());

    if (deep) {
        for (auto* child : srcEl->childNodes()) {
            if (Node* childClone = cloneNode(child, true, preserveId))
                clone->appendChild(childClone);
        }
        if (srcEl->templateContent()) {
            if (Node* fragClone = cloneNode(srcEl->templateContent(), true, preserveId)) {
                if (auto* fEl = dynamic_cast<Element*>(fragClone)) {
                    clone->setTemplateContent(fEl);
                }
            }
        }
    }

    // Children first: the hook stamps <select> state onto cloned <option>s.
    if (elementClonedCb_) elementClonedCb_(this, srcEl, clone);

    return clone;
}

ShadowRoot* Document::allocateShadowRoot(Element* host, ShadowRoot::Mode mode) {
    return allocateNode<ShadowRoot>(host, mode);
}

// Move one node's ownership record from `src` into this document. The subtree
// walk is the caller's job (adoptNode) so the whole tree moves before any
// callback observes a half-adopted state.
void Document::adoptOne(Node* node, Document* src) {
    if (src && src != this) {
        // Element ids are per-document: drop the source's registration before
        // the node stops belonging to it, add ours after.
        if (node->nodeType() == NodeType::Element) {
            auto* elem = static_cast<Element*>(node);
            if (!elem->id().empty()) src->unregisterElementId(elem->id(), elem);
            if (elem == src->focusedElement_) src->focusedElement_ = nullptr;
        }
        // Live ranges in the source document can't span into another document.
        for (auto* r : src->liveRanges_) r->onNodeDestroyed(node);

        auto it = src->ownedNodes_.find(node);
        if (it != src->ownedNodes_.end()) {
            ownedNodes_[node] = std::move(it->second);
            src->ownedNodes_.erase(it);
        }
    }
    node->setDocument(this);
    if (node->nodeType() == NodeType::Element) {
        auto* elem = static_cast<Element*>(node);
        if (!elem->id().empty()) registerElementId(elem->id(), elem);
        // Styles were resolved against the source document's cascade.
        elem->markDirty();
        elem->markStructureDirty();
    }
}

Node* Document::adoptNode(Node* node) {
    if (!node) return nullptr;
    Document* src = node->document();
    if (src == this) {
        // Same document: spec still removes the node from its parent.
        if (auto* p = node->parentNode()) p->removeChild(node);
        return node;
    }
    if (auto* p = node->parentNode()) p->removeChild(node);

    // Deepest-last walk: every node in the subtree changes owner.
    std::vector<Node*> stack{node};
    while (!stack.empty()) {
        Node* n = stack.back();
        stack.pop_back();
        for (auto* child : n->childNodes()) stack.push_back(child);
        adoptOne(n, src);
    }
    markDirty();
    return node;
}

void Document::freeNode(Node* node) {
    if (!node) return;
    auto kids = node->childNodes();
    for (auto* child : kids) {
        freeNode(child);
    }
    // Clear any live Range endpoints that still reference this node. Paths
    // like setTextContent and innerHTML-replacement detach children without
    // firing notifyNodeRemoved, so endpoints can outlive the node they
    // point at — this is the last chance to break that reference before
    // the memory is (eventually) freed from pendingFrees_.
    for (auto* r : liveRanges_) {
        r->onNodeDestroyed(node);
    }
    // Unregister element id from the lookup map
    if (node->nodeType() == NodeType::Element) {
        auto* elem = static_cast<Element*>(node);
        std::string id = elem->id();
        if (!id.empty())
            unregisterElementId(id, elem);
        // The focused element is about to be deallocated — drop the document's
        // reference so activeElement() can never hand out freed memory. (The
        // engine polls activeElement() on every mouse event: a button that
        // removes itself from its own click handler — focused by the very
        // mousedown that triggered it — would otherwise dangle here and crash
        // the next mousemove. Engine::reapDeadInputPointers scrubs the
        // engine's own cached pointers but not this document-owned one.)
        if (elem == focusedElement_) focusedElement_ = nullptr;
        // And the top layer, which the paint and hit-test paths walk.
        if (!topLayer_.empty()) removeFromTopLayer(elem);
        // Same reasoning, one layer up: the CSS transition and animation
        // managers index by raw Element* and dereference the key on the next
        // tick (markDirty() when a transition/animation completes, and the
        // queued transitionend/animationend carries the pointer to
        // dispatchEvent). An element removed while a transition was still
        // running therefore crashed the process on the tick after this node's
        // deferred free drained — which is what a fading toast that removes
        // itself does every single time. Removal cancels both per CSS, so
        // there is no event still owed.
        if (transitionManager_) transitionManager_->forgetElement(elem);
        if (animationManager_) animationManager_->forgetElement(elem);
    }
    // Let the JS layer drop this node's wrapper (raw Element* + elem-map entry)
    // while the memory is still valid. Children were handled by the recursion
    // above. Without this, wrappers created for nodes freed via paths that
    // don't call invalidateWrapper (innerHTML/textContent replacement, range
    // extraction, the orphan-fragment sweep) outlive drainPendingFrees() and
    // dangle — a later property access or sweepOrphanedWrappers() then reads
    for (NodeObserver obs : nodeFreedObservers_) obs(this, node);

    // Move the owning unique_ptr into pendingFrees_ rather than destroying
    // it now. The raster thread may still hold a raw pointer from an
    // in-flight traversal; delaying destruction until both threads are
    // idle keeps those pointers valid for their brief lifetime.
    auto it = ownedNodes_.find(node);
    if (it != ownedNodes_.end()) {
        pendingFrees_.push_back(std::move(it->second));
        pendingSet_.insert(node);
        ownedNodes_.erase(it);
    }
}

void Document::addNodeFreedObserver(NodeObserver cb) {
    if (!cb) return;
    for (NodeObserver existing : nodeFreedObservers_)
        if (existing == cb) return;   // idempotent: installing twice is one hook
    nodeFreedObservers_.push_back(cb);
}

void Document::addMutationObserver(MutationObserverFn cb) {
    if (!cb) return;
    for (MutationObserverFn existing : mutationObservers_)
        if (existing == cb) return;   // idempotent: installing twice is one hook
    mutationObservers_.push_back(cb);
}

void Document::removeMutationObserver(MutationObserverFn cb) {
    mutationObservers_.erase(
        std::remove(mutationObservers_.begin(), mutationObservers_.end(), cb),
        mutationObservers_.end());
}

Document::NodeRetainQuery Document::s_retainQuery = nullptr;

namespace {

bool anyRetained(const Node* n, Document::NodeRetainQuery q) {
    if (q && q(n)) return true;
    for (const Node* c : n->childNodes())
        if (anyRetained(c, q)) return true;
    return false;
}

} // namespace

void Document::freeUnlessRetained(Node* root) {
    if (!root || root->parentNode()) return;
    if (!anyRetained(root, s_retainQuery)) {
        freeNode(root);
        return;
    }
    // The host holds the root: it keeps its whole subtree.
    if (s_retainQuery && s_retainQuery(root)) return;
    // Something below the root is held: keep those parts, free the rest.
    // Unparented directly, as releaseChildrenPreservingElements does — nobody
    // observes a node the host has never seen, so there is no removal to
    // report.
    std::vector<Node*> kids = root->childNodes();
    for (Node* k : kids) k->setParent(nullptr);
    root->childNodes().clear();
    for (Node* k : kids) freeUnlessRetained(k);
    freeNode(root);
}

void Document::notifyMutation(const MutationNotice& notice) {
    if (muteMutationNotices_ > 0) return;
    // By value into a local copy of the list, not by reference into the member:
    // an observer is allowed to disconnect itself, and on the web that is the
    // FIRST thing a one-shot observer does.
    std::vector<MutationObserverFn> snapshot = mutationObservers_;
    for (MutationObserverFn obs : snapshot) obs(this, notice);
}

void Document::removeNodeFreedObserver(NodeObserver cb) {
    nodeFreedObservers_.erase(
        std::remove(nodeFreedObservers_.begin(), nodeFreedObservers_.end(), cb),
        nodeFreedObservers_.end());
}

void Document::drainPendingFrees() {
    pendingFrees_.clear();
    pendingSet_.clear();
}

void Document::forEachLiveElement(const std::function<void(Element*)>& fn) {
    if (!fn) return;

    for (auto& [n, _] : ownedNodes_) {
        if (n && n->nodeType() == NodeType::Element)
            fn(static_cast<Element*>(n));
    }

    // pendingFrees_ holds detached subtree roots whose memory is still alive.
    // Their descendants are owned by the root (not necessarily still listed in
    // ownedNodes_), so walk each subtree explicitly.
    std::function<void(Node*)> walk = [&](Node* n) {
        if (!n) return;
        if (n->nodeType() == NodeType::Element)
            fn(static_cast<Element*>(n));
        for (Node* child : n->childNodes()) walk(child);
    };
    for (auto& root : pendingFrees_) walk(root.get());
}

bool Document::ownsNode(const Node* n) const {
    if (!n) return false;
    return ownedNodes_.find(const_cast<Node*>(n)) != ownedNodes_.end();
}

bool Document::isNodeLive(const Node* n) const {
    if (!n) return false;
    Node* key = const_cast<Node*>(n);
    return ownedNodes_.find(key) != ownedNodes_.end() ||
           pendingSet_.find(key) != pendingSet_.end();
}

Node* Document::resolveNode(const Node* ptr, uint32_t id) const {
    if (!ptr) return nullptr;
    Node* key = const_cast<Node*>(ptr);
    // Pointer-value lookup first — never dereference `ptr` until it is proven
    // to be a node this document keeps alive. The id check then closes the
    // ABA hole isNodeLive alone has: if the allocator reused the address for
    // a *new* node, its nodeId (globally monotonic, never recycled) differs
    // and the stale handle resolves to null instead of the impostor.
    if (ownedNodes_.find(key) == ownedNodes_.end() &&
        pendingSet_.find(key) == pendingSet_.end())
        return nullptr;
    return key->nodeId() == id ? key : nullptr;
}

} // namespace bro::dom
