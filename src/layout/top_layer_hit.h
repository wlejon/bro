#pragma once

// Hit testing a document that has a top layer (dom::Document::topLayer).
//
// The entries paint above everything (draw_traversal_top_layer.cpp), so they
// are tested first, topmost first, each as a subtree on its own. A modal
// entry (a dialog opened with showModal) also blocks the rest of the page:
// a point outside it lands on its ::backdrop, whose events go to the element
// itself, and nothing beneath is ever hit — the page outside is inert.

namespace htmlayout::layout { class LayoutNode; }
namespace bro::dom { class Document; class Element; }

namespace bro::layout {

struct TopLayerHit {
    // True when the top layer decided the target; `element` is then the hit
    // (the element under the point, or the blocking modal entry). False when
    // the caller should hit test the document as usual.
    bool handled = false;
    dom::Element* element = nullptr;
};

// `x`, `y` are in the same space the caller would hand htmlayout's hitTest
// for `root` (document space). `scrollY` is the document's scroll, which a
// position:fixed entry does not move with.
TopLayerHit hitTestTopLayer(dom::Document* doc, htmlayout::layout::LayoutNode* root,
                            float x, float y, float scrollY = 0.0f);

}  // namespace bro::layout
