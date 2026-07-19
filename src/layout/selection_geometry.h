#pragma once
#include "layout/layout_node_adapter.h"
#include "layout/text_geometry.h"
#include "dom/text_node.h"
#include <vector>

// Bridge between bro::dom::Range / bro::dom::Selection and
// htmlayout::layout::{hitTestText, getCaretRect, getSelectionRects}.
//
// htmlayout's geometry queries operate on LayoutNode*; these helpers resolve
// bro::dom::TextNode ↔ LayoutNodeAdapter* and return coordinates in the
// absolute layout space the renderer uses.

namespace bro::dom { class Document; class Node; }
namespace htmlayout::layout { struct TextMetrics; struct Rect; }

namespace bro::layout {

struct TextHit {
    dom::TextNode* textNode = nullptr;
    int            srcOffset = 0;
};

// (x, y) are document-space coordinates (y already adjusted for scroll).
//
// `scope` limits the search to one element's subtree. A hit that misses every
// run still resolves — to the nearest run — and without a scope "nearest" is
// measured across the WHOLE document, so a press in the blank right-hand part
// of a wide editable box lands on whatever unrelated text happens to be
// closest, typically the line below it. Pass the editing host to keep the
// caret inside the element the user actually pressed. Null searches the
// document, which is what a press outside any editable region wants.
TextHit hitTestText(dom::Document* doc, float x, float y,
                    htmlayout::layout::TextMetrics& metrics,
                    dom::Element* scope = nullptr);

// Caret geometry for (textNode, srcOffset) within document.
// Returns false if the text node has no placed runs.
bool getCaretRect(dom::Document* doc, dom::TextNode* textNode, int srcOffset,
                  htmlayout::layout::TextMetrics& metrics,
                  float& x, float& y, float& height);

// Per-line highlight rectangles for a (startNode/off → endNode/off) range.
// Accepts any bro::dom::Node — Element ranges are approximated by walking
// into the first/last descendant text node. Non-text ranges return empty.
std::vector<htmlayout::layout::Rect>
getSelectionRects(dom::Document* doc,
                  dom::Node* startNode, int startOff,
                  dom::Node* endNode,   int endOff,
                  htmlayout::layout::TextMetrics& metrics);

} // namespace bro::layout
