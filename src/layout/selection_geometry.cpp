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

    if (idx < static_cast<int>(kids.size())) {
        auto* t = descend(kids[idx], !preferLeading);
        if (t) {
            outNode = t;
            outOff = preferLeading ? 0 : static_cast<int>(t->length());
            return;
        }
    }
    if (idx > 0) {
        auto* t = descend(kids[idx - 1], true);
        if (t) {
            outNode = t;
            outOff = static_cast<int>(t->length());
        }
    }
}

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

} // namespace

TextHit hitTestText(dom::Document* doc, float x, float y,
                    htmlayout::layout::TextMetrics& metrics) {
    TextHit result;
    if (!doc) return result;
    auto* root = doc->layoutRoot();
    if (!root) return result;
    auto hit = htmlayout::layout::hitTestText(root, x, y, metrics);
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
                  htmlayout::layout::TextMetrics& metrics) {
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
                                                eAdapter, eOff, metrics);
}

} // namespace bro::layout
