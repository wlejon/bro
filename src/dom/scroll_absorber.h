#pragma once

namespace bro::dom {

class Element;

// Who may take an element's scroll with no paint (the engine, for a scroller
// composited as a scroll layer: engine/scroll_layers.h). A document asks it
// on every write of an offset that changes it (Document::setScrollAbsorber);
// true: it moved the picture, and the write stamps and marks nothing
// (Element::scrollAbsorbed).
class ScrollAbsorber {
public:
    virtual ~ScrollAbsorber() = default;
    virtual bool absorbScroll(Element* e, float scrollLeft, float scrollTop) = 0;
};

}  // namespace bro::dom
