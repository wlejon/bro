#include "engine/hit_testing.h"
#include "css/transform.h"
#include "dom/element.h"
#include "dom/node.h"

#include <string>

namespace bro::engine {

dom::Element* hitTestElement(dom::Element* elem, float x, float y,
                             float offsetX, float offsetY) {
    if (!elem) return nullptr;

    auto& style = elem->computedStyle();

    // Skip invisible / non-interactive elements
    {
        auto it = style.find("display");
        if (it != style.end() && it->second == "none") return nullptr;
    }
    {
        auto it = style.find("pointer-events");
        if (it != style.end() && it->second == "none") return nullptr;
    }
    {
        auto it = style.find("visibility");
        if (it != style.end() && it->second == "hidden") return nullptr;
    }

    auto& box = elem->layoutBox();

    // Absolute content position
    float absX = box.contentRect.x + offsetX;
    float absY = box.contentRect.y + offsetY;

    // Border box bounds (also the reference box for transforms)
    float bx = absX - box.padding.left - box.border.left;
    float by = absY - box.padding.top  - box.border.top;
    float bw = box.fullWidth();
    float bh = box.fullHeight();

    // Apply CSS transform: map the test point through the inverse transform
    // around transform-origin.
    float testX = x, testY = y;
    {
        auto it = style.find("transform");
        if (it != style.end() && !it->second.empty() && it->second != "none") {
            auto mat = htmlayout::css::parseTransform(it->second, bw, bh);
            if (!mat.isIdentity()) {
                float ox, oy;
                auto oIt = style.find("transform-origin");
                std::string_view originVal =
                    (oIt != style.end()) ? std::string_view(oIt->second)
                                         : std::string_view();
                htmlayout::css::parseTransformOrigin(originVal, bw, bh, ox, oy);
                // Build full transform: T(origin) * M * T(-origin)
                htmlayout::css::Matrix2D toOrigin{1,0,0,1, bx+ox, by+oy};
                htmlayout::css::Matrix2D fromOrigin{1,0,0,1, -(bx+ox), -(by+oy)};
                auto full = toOrigin * mat * fromOrigin;
                htmlayout::css::Matrix2D inv;
                if (full.invert(inv)) {
                    testX = inv.a * x + inv.c * y + inv.e;
                    testY = inv.b * x + inv.d * y + inv.f;
                }
            }
        }
    }

    // The document element (<html>) accepts hits across the entire viewport,
    // so stray clicks outside any laid-out content still resolve — matches
    // browser behavior.
    bool isDocumentElement = (elem->parentNode() == nullptr ||
                              elem->parentNode()->nodeType() == dom::NodeType::Document);

    // Determine if this element clips child hit testing to its border box.
    // Elements with overflow:visible (the default) allow absolutely positioned
    // children to overflow, so we must test children even when the click is
    // outside this element's bounds.
    bool clipsChildren = false;
    if (!isDocumentElement) {
        auto ovIt = style.find("overflow");
        if (ovIt != style.end() && ovIt->second != "visible")
            clipsChildren = true;
        if (!clipsChildren) {
            auto ovxIt = style.find("overflow-x");
            auto ovyIt = style.find("overflow-y");
            if ((ovxIt != style.end() && ovxIt->second != "visible") ||
                (ovyIt != style.end() && ovyIt->second != "visible"))
                clipsChildren = true;
        }
    }

    bool insideBounds = isDocumentElement ||
        (testX >= bx && testX < bx + bw && testY >= by && testY < by + bh);

    if (clipsChildren && !insideBounds)
        return nullptr;

    // Adjust child offset for element-level scroll
    float childOffsetX = absX;
    float childOffsetY = absY - elem->scrollTopValue();

    // Composed children (shadow DOM + slot replacement), checked in reverse
    // order for correct z-ordering (last-painted = topmost)
    auto composedChildren = elem->composedChildNodes();
    for (int i = static_cast<int>(composedChildren.size()) - 1; i >= 0; --i) {
        auto* hit = hitTestNode(composedChildren[i], testX, testY,
                                childOffsetX, childOffsetY);
        if (hit) return hit;
    }

    if (insideBounds)
        return elem;
    return nullptr;
}

dom::Element* hitTestNode(dom::Node* node, float x, float y,
                          float offsetX, float offsetY) {
    if (!node || node->nodeType() != dom::NodeType::Element) return nullptr;
    return hitTestElement(static_cast<dom::Element*>(node), x, y, offsetX, offsetY);
}

} // namespace bro::engine
