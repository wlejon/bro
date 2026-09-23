#include "layout/top_layer_hit.h"

#include "dom/document.h"
#include "dom/element.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"

#include <vector>

namespace bro::layout {

namespace {

// The layout node an element generated, or null (not rendered). A depth-first
// walk — run only while the top layer is non-empty, and only for its entries.
htmlayout::layout::LayoutNode* layoutNodeOf(htmlayout::layout::LayoutNode* root,
                                            const dom::Element* el) {
    std::vector<htmlayout::layout::LayoutNode*> stack{root};
    while (!stack.empty()) {
        auto* n = stack.back();
        stack.pop_back();
        if (!n) continue;
        auto* a = static_cast<LayoutNodeAdapter*>(n);
        if (a->element() == el) return n;
        for (auto* c : n->children()) stack.push_back(c);
    }
    return nullptr;
}

}  // namespace

TopLayerHit hitTestTopLayer(dom::Document* doc, htmlayout::layout::LayoutNode* root,
                            float x, float y, float scrollY) {
    TopLayerHit out;
    if (!doc || !root || doc->topLayer().empty()) return out;
    const auto entries = doc->topLayer();   // copy: nothing below may mutate it, but be safe
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        dom::Element* el = it->element;
        if (auto* node = layoutNodeOf(root, el)) {
            const auto& style = el->computedStyle();
            auto pos = style.find("position");
            const bool fixed = pos != style.end() && pos->second == "fixed";
            auto* hit = htmlayout::layout::hitTestSubtree(node, x, fixed ? y - scrollY : y);
            if (dom::Element* hitEl = LayoutNodeAdapter::elementFor(hit)) {
                out.handled = true;
                out.element = hitEl;
                return out;
            }
        }
        if (it->modal) {
            // The backdrop: everything below is inert, the element takes it.
            out.handled = true;
            out.element = el;
            return out;
        }
    }
    return out;
}

}  // namespace bro::layout
