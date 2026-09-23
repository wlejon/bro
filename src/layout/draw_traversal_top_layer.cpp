// Painting the top layer (dom::Document::topLayer).
//
// buildStackingContextTree lifts each top-layer element's subtree out of its
// ancestors into a root-level stacking context (topLayerSCs_), so none of the
// ancestors' z-index, stacking contexts, transforms, opacity or overflow clips
// apply to it. draw() then paints those after the whole document, in top-layer
// order (the last entry is the topmost), each directly above its ::backdrop.
//
// The ::backdrop is a box the size of the viewport (the UA sheet makes it
// position:fixed with inset:0); its background-color and opacity paint.

#include "layout/draw_traversal.h"
#include "layout/pseudo_style.h"
#include "dom/document.h"
#include "dom/element.h"

namespace bro::layout {

void DrawTraversal::topLayerOffset(dom::Element* elem, float& offX, float& offY) const {
    // The parent's content origin as layout computed it: the plain sum of
    // contentRect origins up the layout-parent chain (htmlayout positions a
    // fixed box against exactly this, with no ancestor scroll in it).
    float x = 0.0f, y = 0.0f;
    for (dom::Element* p = elem->layoutParent(); p; p = p->layoutParent()) {
        const auto& b = p->layoutBox();
        x += b.contentRect.x;
        y += b.contentRect.y;
    }
    // Draw space is the viewport scrolled by the document: a fixed box stays
    // put, anything else moves with the page.
    const auto& style = elem->computedStyle();
    auto pos = style.find("position");
    const bool fixed = pos != style.end() && pos->second == "fixed";
    if (!fixed) {
        x += rootOffsetX_;
        y += rootOffsetY_;
    }
    offX = x;
    offY = y;
}

void DrawTraversal::paintBackdrop(dom::Element* elem) {
    auto style = resolveStyledPseudo(elem, "backdrop");
    if (style.empty()) return;
    auto disp = style.find("display");
    if (disp != style.end() && disp->second == "none") return;
    auto vis = style.find("visibility");
    if (vis != style.end() && vis->second == "hidden") return;

    bromath::Color bg;
    if (!pseudoColor(style, "background-color", bg) || bg.a <= 0.0f) return;
    bg.a *= pseudoOpacity(style);
    if (bg.a <= 0.0f) return;
    renderer_->fillRect(0.0f, static_cast<float>(viewportTop_),
                        static_cast<float>(viewportW_), static_cast<float>(viewportH_), bg);
}

void DrawTraversal::paintTopLayer(dom::Element* root) {
    if (topLayerSCs_.empty() || !root) return;
    dom::Document* doc = root->document();
    if (!doc) return;
    // The paint may not touch the list; iterate a copy of the element order.
    std::vector<dom::Element*> order;
    order.reserve(doc->topLayer().size());
    for (const auto& e : doc->topLayer()) order.push_back(e.element);
    for (dom::Element* el : order) {
        auto it = topLayerSCs_.find(el);
        if (it == topLayerSCs_.end()) continue;   // not rendered (display:none)
        // The backdrop is base content, never part of a promoted layer.
        if (paintMode_ != PaintMode::PromotedOnly) paintBackdrop(el);
        paintStackingContext(it->second.get());
    }
}

}  // namespace bro::layout
