#include "engine/overflow.h"
#include "engine/scrollbar.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_scroll.h"
#include "dom/shadow_root.h"

#include <algorithm>
#include <unordered_map>

namespace bro::engine {

std::string getOverflowY(const htmlayout::css::ComputedStyle& style) {
    return dom::usedOverflow(style, false);
}

std::string getOverflowX(const htmlayout::css::ComputedStyle& style) {
    return dom::usedOverflow(style, true);
}

bool overflowClips(const std::string& ov) {
    return ov == "hidden" || ov == "scroll" || ov == "auto";
}

bool overflowScrollable(const std::string& ov) {
    return ov == "scroll" || ov == "auto";
}

bool setScrollOffsetClamped(dom::Element* el, bool horizontal, float offset) {
    if (!el) return false;
    if (horizontal) {
        const float prev = el->scrollLeftValue();
        const float next = std::clamp(offset, 0.0f, dom::maxScrollLeftOf(el));
        el->setScrollLeftValue(next);
        return next != prev;
    }
    const float prev = el->scrollTopValue();
    const float next = std::clamp(offset, 0.0f, dom::maxScrollTopOf(el));
    el->setScrollTopValue(next);
    return next != prev;
}

dom::Element* wheelScrollChain(dom::Element* target, bool horizontal, float delta,
                               bool& moved) {
    moved = false;
    if (delta == 0.0f) return nullptr;
    for (dom::Element* el = target; el; el = composedParent(el)) {
        const auto& style = el->computedStyle();
        if (!overflowScrollable(horizontal ? getOverflowX(style) : getOverflowY(style)))
            continue;
        const float max = horizontal ? dom::maxScrollLeftOf(el) : dom::maxScrollTopOf(el);
        if (max <= 0.0f) continue;
        const float prev = horizontal ? el->scrollLeftValue() : el->scrollTopValue();
        // Pinned at the edge the delta pushes toward: chain to the next one.
        const bool canScroll = (delta < 0.0f) ? (prev > 0.5f) : (prev < max - 0.5f);
        if (!canScroll) continue;
        moved = setScrollOffsetClamped(el, horizontal, prev + delta);
        return el;
    }
    return nullptr;
}

namespace {

bool isDisplayNone(const dom::Element* el) {
    const auto& style = el->computedStyle();
    auto it = style.find("display");
    return it != style.end() && it->second == "none";
}

// One scroller's offsets, brought back within its content. True if either moved.
bool clampOne(dom::Element* el) {
    const auto& style = el->computedStyle();
    bool moved = false;
    // A scroller only holds an offset if it clips; overflow:visible never does.
    if (el->scrollTopValue() != 0.0f && overflowClips(getOverflowY(style))) {
        float clamped = dom::clampedScrollTopOf(el);
        if (clamped != el->scrollTopValue()) {
            el->setScrollTopValue(clamped);
            moved = true;
        }
    }
    if (el->scrollLeftValue() != 0.0f) {
        // maxScrollLeftOf is 0 for a box that does not scroll horizontally,
        // so a stale offset on a box that stopped being a scroller resets too.
        float clamped = dom::clampedScrollLeftOf(el);
        if (clamped != el->scrollLeftValue()) {
            el->setScrollLeftValue(clamped);
            moved = true;
        }
    }
    return moved;
}

}  // namespace

bool clampScrollOffsets(dom::Element* root, std::vector<dom::Element*>* changed) {
    if (!root || !root->document()) return false;
    dom::Document* doc = root->document();
    // Only an element holding an offset can need clamping: the document keeps
    // those (Document::scrolledElements), so this looks at a handful rather
    // than walking every element (which cost a one-class change in a
    // 10,000-element document most of its frame). A copy: clamping to 0 takes
    // an element out of the set.
    const std::vector<dom::Element*> scrolled(doc->scrolledElements().begin(), doc->scrolledElements().end());
    bool moved = false;
    for (dom::Element* el : scrolled) {
        // Freed, or adopted into another document: not ours to touch.
        if (!doc->ownsNode(el) || el->document() != doc) {
            doc->forgetScrolled(el);
            continue;
        }
        // Under `root` in the composed tree, and rendered: an offset under
        // display:none is remembered, not clamped against a stale box; one
        // in a detached tree waits for it to come back.
        bool under = false, hidden = false;
        for (dom::Element* e = el; e; e = composedParent(e)) {
            if (isDisplayNone(e)) { hidden = true; break; }
            if (e == root) { under = true; break; }
        }
        if (!under || hidden) continue;
        if (clampOne(el)) {
            if (changed) changed->push_back(el);
            moved = true;
        }
    }
    return moved;
}

ElementScrollbarLayout layoutElementScrollbars(dom::Element* elem,
                                               float contentX, float contentY,
                                               const Scrollbar& scrollbar) {
    ElementScrollbarLayout out;
    if (!elem) return out;
    const auto& style = elem->computedStyle();
    const auto& lbox = elem->layoutBox();

    const float maxST = overflowScrollable(getOverflowY(style)) ? dom::maxScrollTopOf(elem) : 0.0f;
    const float maxSL = overflowScrollable(getOverflowX(style)) ? dom::maxScrollLeftOf(elem) : 0.0f;
    if (maxST <= 0.0f && maxSL <= 0.0f) return out;

    const float bx = contentX - lbox.padding.left - lbox.border.left;
    const float by = contentY - lbox.padding.top - lbox.border.top;
    const float bw = lbox.fullWidth();
    const float bh = lbox.fullHeight();
    const auto& es = scrollbar.style();
    // When both bars show, each leaves the other its corner.
    const float corner = (maxST > 0.0f && maxSL > 0.0f) ? es.width + es.margin : 0.0f;

    if (maxST > 0.0f) {
        const float viewH = lbox.contentRect.height;
        out.v = scrollbar.layout(bx + bw - es.width - es.margin, by, bh - corner,
                                 viewH + maxST, viewH, dom::clampedScrollTopOf(elem));
    }
    if (maxSL > 0.0f) {
        const float viewW = lbox.contentRect.width;
        out.h = scrollbar.layoutHorizontal(bx, by + bh - es.width - es.margin, bw - corner,
                                           viewW + maxSL, viewW, dom::clampedScrollLeftOf(elem));
    }
    return out;
}

namespace {

// The scroll range a bar on `horizontal`'s axis covers: content and view length.
void scrollExtent(dom::Element* el, bool horizontal, float& content, float& view) {
    const auto& box = el->layoutBox();
    view = horizontal ? box.contentRect.width : box.contentRect.height;
    content = view + (horizontal ? dom::maxScrollLeftOf(el) : dom::maxScrollTopOf(el));
}

}  // namespace

bool pageElementScrollbar(dom::Element* el, const Scrollbar& scrollbar,
                          const ScrollbarMetrics& m, float along) {
    if (!el || !m.visible) return false;
    float content = 0.0f, view = 0.0f;
    scrollExtent(el, m.horizontal, content, view);
    return setScrollOffsetClamped(el, m.horizontal,
                                  scrollbar.scrollToPosition(along, content, view, m));
}

bool dragElementScrollbar(dom::Element* el, const Scrollbar& scrollbar, float along) {
    if (!el || !scrollbar.isDragging()) return false;
    const bool horizontal = scrollbar.dragHorizontal();
    // Only the bar's length and thumb size matter to a drag (it moves by the
    // pointer's travel since beginDrag), so the origin is immaterial.
    const ElementScrollbarLayout bars = layoutElementScrollbars(el, 0.0f, 0.0f, scrollbar);
    const ScrollbarMetrics& m = horizontal ? bars.h : bars.v;
    if (!m.visible) return false;
    float content = 0.0f, view = 0.0f;
    scrollExtent(el, horizontal, content, view);
    return setScrollOffsetClamped(el, horizontal,
                                  scrollbar.updateDrag(along, content, view, m));
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

    // Recurse into composed children FIRST to find the deepest match. Clamped
    // offsets, matching what was painted (drawElementScrollbars uses the same
    // layout), so the scrollbar we hit-test is the one on screen.
    float childOffsetX = absX - dom::clampedScrollLeftOf(elem);
    float childOffsetY = absY - dom::clampedScrollTopOf(elem);
    dom::Element* hit = nullptr;
    elem->forEachComposedChild([&](dom::Element* child) {
        if (!hit) {
            hit = findElementScrollbarHit(child, x, y,
                childOffsetX, childOffsetY, scrollbar, outMetrics);
        }
    });
    if (hit) return hit;

    const ElementScrollbarLayout bars = layoutElementScrollbars(elem, absX, absY, scrollbar);
    for (const ScrollbarMetrics* m : {&bars.v, &bars.h}) {
        if (scrollbar.hitTest(x, y, *m)) {
            outMetrics = *m;
            return elem;
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
