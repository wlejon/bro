#include "engine/overflow.h"
#include "engine/scrollbar.h"
#include "dom/element.h"
#include "dom/shadow_root.h"

#include <algorithm>
#include <unordered_map>

namespace bro::engine {

std::string getOverflowY(const htmlayout::css::ComputedStyle& style) {
    auto oyIt = style.find("overflow-y");
    if (oyIt != style.end()) return oyIt->second;
    auto oIt = style.find("overflow");
    if (oIt != style.end()) return oIt->second;
    return "visible";
}

bool overflowClips(const std::string& ov) {
    return ov == "hidden" || ov == "scroll" || ov == "auto";
}

bool overflowScrollable(const std::string& ov) {
    return ov == "scroll" || ov == "auto";
}

float maxScrollTop(dom::Element* el) {
    auto& box = el->layoutBox();
    return std::max(0.0f, box.naturalHeight - box.contentRect.height);
}

bool clampScrollOffsets(dom::Element* root, std::vector<dom::Element*>* changed) {
    if (!root) return false;
    auto& style = root->computedStyle();
    {
        auto it = style.find("display");
        if (it != style.end() && it->second == "none") return false;
    }

    bool moved = false;
    // A scroller only holds an offset if it clips; overflow:visible never does.
    if (root->scrollTopValue() != 0.0f && overflowClips(getOverflowY(style))) {
        float clamped = std::clamp(root->scrollTopValue(), 0.0f, maxScrollTop(root));
        if (clamped != root->scrollTopValue()) {
            root->setScrollTopValue(clamped);
            if (changed) changed->push_back(root);
            moved = true;
        }
    }
    root->forEachComposedChild([&](dom::Element* child) {
        if (clampScrollOffsets(child, changed)) moved = true;
    });
    return moved;
}

dom::Element* findElementScrollbarHit(
    dom::Element* elem, float x, float y,
    float offsetX, float offsetY,
    Scrollbar& scrollbar, ScrollbarMetrics& outMetrics)
{
    if (!elem) return nullptr;
    auto& style = elem->computedStyle();
    {
        auto it = style.find("display");
        if (it != style.end() && it->second == "none") return nullptr;
    }

    auto& lbox = elem->layoutBox();
    float absX = lbox.contentRect.x + offsetX;
    float absY = lbox.contentRect.y + offsetY;

    // Clamped to match what was painted (drawElementScrollbars uses the same
    // clamp), so the scrollbar we hit-test is the one on screen.
    float scrollTop = std::clamp(elem->scrollTopValue(), 0.0f, maxScrollTop(elem));

    // Recurse into composed children FIRST to find the deepest match.
    float childOffsetX = absX;
    float childOffsetY = absY - scrollTop;
    dom::Element* hit = nullptr;
    elem->forEachComposedChild([&](dom::Element* child) {
        if (!hit) {
            hit = findElementScrollbarHit(child, x, y,
                childOffsetX, childOffsetY, scrollbar, outMetrics);
        }
    });
    if (hit) return hit;

    std::string ov = getOverflowY(style);
    if (overflowScrollable(ov)) {
        float maxST = maxScrollTop(elem);
        if (maxST > 0) {
            float viewH = lbox.contentRect.height;
            float contentH = viewH + maxST;
            float bx = absX - lbox.padding.left - lbox.border.left;
            float by = absY - lbox.padding.top - lbox.border.top;
            float bw = lbox.fullWidth();
            float bh = lbox.fullHeight();

            auto& es = scrollbar.style();
            auto m = scrollbar.layout(
                bx + bw - es.width - es.margin,
                by, bh, contentH, viewH,
                scrollTop);
            if (scrollbar.hitTest(x, y, m)) {
                outMetrics = m;
                return elem;
            }
        }
    }
    return nullptr;
}

dom::Element* composedParent(dom::Element* el) {
    if (!el) return nullptr;
    auto* p = el->parentNode();
    if (!p) return nullptr;
    if (p->nodeType() == dom::NodeType::Element)
        return static_cast<dom::Element*>(p);
    // Parent is a ShadowRoot — cross to the host element
    if (p->nodeType() == dom::NodeType::DocumentFragment) {
        auto* sr = dynamic_cast<dom::ShadowRoot*>(p);
        if (sr) return sr->host();
    }
    return nullptr;
}

} // namespace bro::engine
