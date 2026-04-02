#include "engine/hit_testing.h"
#include "dom/element.h"
#include "dom/node.h"

#include <cmath>
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

    // Apply transform (translate only)
    float testX = x, testY = y;
    {
        auto it = style.find("transform");
        if (it != style.end() && !it->second.empty() && it->second != "none") {
            float tx = 0, ty = 0;
            const auto& v = it->second;
            auto pos = v.find("translate(");
            if (pos != std::string::npos) {
                auto inner = v.substr(pos + 10);
                char* end = nullptr;
                tx = std::strtof(inner.c_str(), &end);
                if (end && (*end == ',' || *end == ' ')) {
                    ty = std::strtof(end + 1, nullptr);
                }
            } else {
                pos = v.find("translateY(");
                if (pos != std::string::npos) {
                    ty = std::strtof(v.c_str() + pos + 11, nullptr);
                }
                pos = v.find("translateX(");
                if (pos != std::string::npos) {
                    tx = std::strtof(v.c_str() + pos + 11, nullptr);
                }
            }
            testX -= tx;
            testY -= ty;
        }
    }

    // Border box bounds
    float bx = absX - box.padding.left - box.border.left;
    float by = absY - box.padding.top  - box.border.top;
    float bw = box.fullWidth();
    float bh = box.fullHeight();
    if (testX < bx || testX >= bx + bw || testY < by || testY >= by + bh)
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

    // No child hit — this element is the target
    return elem;
}

dom::Element* hitTestNode(dom::Node* node, float x, float y,
                          float offsetX, float offsetY) {
    if (!node || node->nodeType() != dom::NodeType::Element) return nullptr;
    return hitTestElement(static_cast<dom::Element*>(node), x, y, offsetX, offsetY);
}

} // namespace bro::engine
