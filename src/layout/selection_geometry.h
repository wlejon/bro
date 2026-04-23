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
TextHit hitTestText(dom::Document* doc, float x, float y,
                    htmlayout::layout::TextMetrics& metrics);

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
