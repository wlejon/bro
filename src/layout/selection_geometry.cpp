#include "layout/selection_geometry.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/text_node.h"
#include <functional>

namespace bro::layout {

namespace {

// Walk the persistent layout tree looking for the adapter whose textNodePtr()
// matches `target`. Text nodes live directly under element adapters; the
// search is O(tree size) but cheap in practice.
LayoutNodeAdapter* findAdapterForText(LayoutNodeAdapter* root, dom::TextNode* target) {
    if (!root) return nullptr;
    if (root->textNodePtr() == target) return root;
    for (auto* c : root->children()) {
        auto* a = static_cast<LayoutNodeAdapter*>(c);
        auto* hit = findAdapterForText(a, target);
        if (hit) return hit;
    }
    return nullptr;
}

// Find adapter for an element so selection can enter/exit its subtree.
LayoutNodeAdapter* findAdapterForElement(LayoutNodeAdapter* root, dom::Element* target) {
    if (!root) return nullptr;
    if (root->element() == target) return root;
    for (auto* c : root->children()) {
        auto* a = static_cast<LayoutNodeAdapter*>(c);
        auto* hit = findAdapterForElement(a, target);
        if (hit) return hit;
    }
    return nullptr;
}

// Given an element and child index, return the DOM text node + offset
// corresponding to that boundary point (used when a Range endpoint lands on
// an Element container). Walks into the first/last descendant text node of
// the adjacent child.
void elementOffsetToText(dom::Element* el, int off, bool preferLeading,
                         dom::TextNode*& outNode, int& outOff) {
    outNode = nullptr;
    outOff = 0;
    if (!el) return;
    const auto& kids = el->childNodes();
    if (kids.empty()) return;
    // If off points past the end, clamp to last child.
    int idx = std::max(0, std::min(off, static_cast<int>(kids.size())));

    // Leading boundary: find first text descendant of child at idx (or the
    // trailing boundary of child idx-1 if idx == size).
    std::function<dom::TextNode*(dom::Node*, bool)> descend =
        [&](dom::Node* n, bool last) -> dom::TextNode* {
        if (!n) return nullptr;
        if (n->nodeType() == dom::NodeType::Text)
            return static_cast<dom::TextNode*>(n);
        const auto& kk = n->childNodes();
        if (kk.empty()) return nullptr;
        if (last) {
            for (auto it = kk.rbegin(); it != kk.rend(); ++it) {
                if (auto* t = descend(*it, true)) return t;
            }
        } else {
            for (auto* k : kk) {
                if (auto* t = descend(k, false)) return t;
            }
        }
        return nullptr;
    };

    // The boundary (el, idx) sits between kids[idx-1] and kids[idx]. A
    // leading (start) boundary resolves to the first text AFTER it; a
    // trailing (end) boundary to the end of the last text BEFORE it. Each
    // falls back to the other side when its own has no text. Resolving an end
    // boundary into kids[idx] instead — its last text, at its length — ran
    // the highlight of `(p, 2)` on through the child at index 2.
    const int n = static_cast<int>(kids.size());
    auto firstTextFrom = [&](int i) -> dom::TextNode* {
        for (; i < n; ++i)
            if (auto* t = descend(kids[i], false)) return t;
        return nullptr;
    };
    auto lastTextBefore = [&](int i) -> dom::TextNode* {
        for (--i; i >= 0; --i)
            if (auto* t = descend(kids[i], true)) return t;
        return nullptr;
    };
    dom::TextNode* after = nullptr;
    dom::TextNode* before = nullptr;
    if (preferLeading) {
        if ((after = firstTextFrom(idx))) { outNode = after; outOff = 0; return; }
        if ((before = lastTextBefore(idx))) {
            outNode = before; outOff = static_cast<int>(before->length()); return;
        }
    } else {
        if ((before = lastTextBefore(idx))) {
            outNode = before; outOff = static_cast<int>(before->length()); return;
        }
        if ((after = firstTextFrom(idx))) { outNode = after; outOff = 0; return; }
    }
}

} // namespace

// Normalize a boundary (Node, offset) to (TextNode, offset). Element
// boundaries are collapsed to the nearest text descendant.
void toTextBoundary(dom::Node* node, int off, bool preferLeading,
                    dom::TextNode*& outNode, int& outOff) {
    if (!node) { outNode = nullptr; outOff = 0; return; }
    if (node->nodeType() == dom::NodeType::Text) {
        outNode = static_cast<dom::TextNode*>(node);
        outOff = off;
        return;
    }
    if (node->nodeType() == dom::NodeType::Element) {
        elementOffsetToText(static_cast<dom::Element*>(node), off, preferLeading,
                            outNode, outOff);
        return;
    }
    outNode = nullptr;
    outOff = 0;
}

TextHit hitTestText(dom::Document* doc, float x, float y,
                    htmlayout::layout::TextMetrics& metrics,
                    dom::Element* scope) {
    TextHit result;
    if (!doc) return result;
    auto* root = doc->layoutRoot();
    if (!root) return result;
    // Restrict the answer to the scope's subtree, but keep walking from the
    // document root: the walk accumulates offsets from wherever it starts, so
    // handing it the subtree would make every run's coordinates relative to
    // that subtree while (x, y) stayed absolute. If the scope has no adapter
    // (display:none, or not laid out yet) the filter is dropped rather than
    // returning nothing — a caret somewhere beats no caret at all.
    const auto* scopeAdapter =
        scope ? findAdapterForElement(root, scope) : nullptr;
    auto hit = htmlayout::layout::hitTestText(root, x, y, metrics, scopeAdapter);
    if (!hit.node) return result;
    auto* adapter = static_cast<LayoutNodeAdapter*>(hit.node);
    if (adapter->isTextNode()) {
        result.textNode = adapter->textNodePtr();
        result.srcOffset = hit.srcOffset;
    }
    return result;
}

bool getCaretRect(dom::Document* doc, dom::TextNode* textNode, int srcOffset,
                  htmlayout::layout::TextMetrics& metrics,
                  float& x, float& y, float& height) {
    if (!doc || !textNode) return false;
    auto* root = doc->layoutRoot();
    auto* adapter = findAdapterForText(root, textNode);
    if (!adapter) return false;
    return htmlayout::layout::getCaretRect(root, adapter, srcOffset,
                                           metrics, x, y, height);
}

std::vector<htmlayout::layout::Rect>
getSelectionRects(dom::Document* doc,
                  dom::Node* startNode, int startOff,
                  dom::Node* endNode,   int endOff,
                  htmlayout::layout::TextMetrics& metrics,
                  bool clipToOverflow) {
    std::vector<htmlayout::layout::Rect> out;
    if (!doc || !startNode || !endNode) return out;

    dom::TextNode* sText = nullptr;
    dom::TextNode* eText = nullptr;
    int sOff = 0, eOff = 0;
    toTextBoundary(startNode, startOff, /*preferLeading=*/true,  sText, sOff);
    toTextBoundary(endNode,   endOff,   /*preferLeading=*/false, eText, eOff);
    if (!sText || !eText) return out;

    auto* root = doc->layoutRoot();
    auto* sAdapter = findAdapterForText(root, sText);
    auto* eAdapter = findAdapterForText(root, eText);
    if (!sAdapter || !eAdapter) return out;

    return htmlayout::layout::getSelectionRects(root, sAdapter, sOff,
                                                eAdapter, eOff, metrics,
                                                clipToOverflow);
}

std::vector<htmlayout::layout::Rect>
inlineFragmentRects(dom::Document* doc, dom::Element* el,
                    htmlayout::layout::TextMetrics& metrics) {
    std::vector<htmlayout::layout::Rect> lines;
    if (!doc || !el) return lines;
    // The element's whole content as one range: htmlayout hands back its text
    // as bands sorted by line and then by x.
    auto bands = getSelectionRects(doc, el, 0, el,
                                   static_cast<int>(el->childNodes().size()),
                                   metrics, /*clipToOverflow=*/false);
    for (const auto& b : bands) {
        // Same line when the band's vertical centre falls inside the line so
        // far — robust to runs of different heights on one line, and to line
        // boxes tighter than their glyphs (whose content areas overlap).
        const float mid = b.y + b.height * 0.5f;
        if (!lines.empty()) {
            auto& l = lines.back();
            if (mid >= l.y && mid <= l.y + l.height) {
                const float x0 = std::min(l.x, b.x), y0 = std::min(l.y, b.y);
                const float x1 = std::max(l.x + l.width, b.x + b.width);
                const float y1 = std::max(l.y + l.height, b.y + b.height);
                l = {x0, y0, x1 - x0, y1 - y0};
                continue;
            }
        }
        lines.push_back(b);
    }
    if (lines.empty()) return lines;

    const auto& box = el->layoutBox();
    const float top = box.padding.top + box.border.top;
    const float bottom = box.padding.bottom + box.border.bottom;
    for (auto& l : lines) {
        l.y -= top;
        l.height += top + bottom;
    }
    const float start = box.padding.left + box.border.left;
    lines.front().x -= start;
    lines.front().width += start;
    lines.back().width += box.padding.right + box.border.right;
    return lines;
}

} // namespace bro::layout
