// The top layer: an ordered set of elements painted over the whole document,
// each above its own ::backdrop, outside every ancestor's stacking context and
// overflow clip, and hit-tested before anything else (CSS Position 4 §top
// layer; HTML's "add an element to the top layer" / "remove ... immediately").
//
// The Document only keeps the set and answers the questions everything else
// asks of it: is this element in it, which entry blocks the rest of the page
// (the topmost modal one), and is a node inert because of that. Painting it
// is DrawTraversal's (paintTopLayer), hit testing it is the engine's
// (Engine::hitTest), and entering or leaving it is the client's — a modal
// dialog today (bronze_host/host_dialog.cpp), Fullscreen and popovers later.

#include "dom/document.h"
#include "dom/shadow_root.h"

#include <algorithm>

namespace bro::dom {

namespace {

// The shadow-including parent: a shadow root's parent is its host.
const Node* composedParent(const Node* n) {
    if (!n) return nullptr;
    if (const Node* p = n->parentNode()) return p;
    if (auto* sr = dynamic_cast<const ShadowRoot*>(n)) return sr->host();
    return nullptr;
}

bool isShadowIncludingInclusiveAncestor(const Node* ancestor, const Node* node) {
    for (const Node* cur = node; cur; cur = composedParent(cur)) {
        if (cur == ancestor) return true;
    }
    return false;
}

}  // namespace

void Document::addToTopLayer(Element* el, bool modal, std::string requiredAttribute) {
    if (!el) return;
    removeFromTopLayer(el);
    topLayer_.push_back({el, modal, std::move(requiredAttribute)});
    // :modal and the paint order both changed; the element also leaves its
    // ancestors' stacking contexts, which is a re-record, not only a restyle.
    el->markDirty();
    markDirty();
}

bool Document::removeFromTopLayer(Element* el) {
    auto it = std::find_if(topLayer_.begin(), topLayer_.end(),
                           [el](const TopLayerEntry& e) { return e.element == el; });
    if (it == topLayer_.end()) return false;
    topLayer_.erase(it);
    // A freed element (freeNode) is no longer safe to touch: only mark the
    // document. A live one re-resolves its :modal styles.
    if (ownsNode(el)) el->markDirty();
    markDirty();
    return true;
}

bool Document::isInTopLayer(const Element* el) const {
    for (const auto& e : topLayer_) {
        if (e.element == el) return true;
    }
    return false;
}

Element* Document::topLayerBlockingElement() const {
    for (auto it = topLayer_.rbegin(); it != topLayer_.rend(); ++it) {
        if (it->modal) return it->element;
    }
    return nullptr;
}

bool Document::isInert(const Node* node) const {
    if (!node || topLayer_.empty()) return false;
    // The blocking element and every entry above it stay live; everything
    // else in the document is inert.
    size_t blocking = topLayer_.size();
    for (size_t i = topLayer_.size(); i-- > 0;) {
        if (topLayer_[i].modal) { blocking = i; break; }
    }
    if (blocking == topLayer_.size()) return false;
    for (size_t i = blocking; i < topLayer_.size(); ++i) {
        if (isShadowIncludingInclusiveAncestor(topLayer_[i].element, node)) return false;
    }
    return true;
}

bool Document::pruneTopLayer() {
    if (topLayer_.empty()) return false;
    std::vector<Element*> gone;
    for (const auto& e : topLayer_) {
        Element* el = e.element;
        bool keep = ownsNode(el) && documentElement_ &&
                    isShadowIncludingInclusiveAncestor(documentElement_, el);
        if (keep && !e.requiredAttribute.empty() && !el->hasAttribute(e.requiredAttribute)) {
            keep = false;
        }
        if (!keep) gone.push_back(el);
    }
    for (Element* el : gone) removeFromTopLayer(el);
    return !gone.empty();
}

}  // namespace bro::dom
