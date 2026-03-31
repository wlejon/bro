#pragma once

#include "layout/box.h"
#include "css/cascade.h"
#include "render/renderer.h"

namespace bro::dom { class Element; }

namespace bro::layout {

// Standalone SVG element renderer.
class ElSvg {
public:
    explicit ElSvg(render::Renderer* renderer);

    void draw(render::Renderer* renderer,
              dom::Element* elem,
              const htmlayout::layout::LayoutBox& box,
              float offsetX, float offsetY);

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    float intrinsicWidth() const { return intrinsicWidth_; }
    float intrinsicHeight() const { return intrinsicHeight_; }
    void parseAttributes();

    void getContentSize(float& w, float& h);

private:
    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    float intrinsicWidth_ = 300;
    float intrinsicHeight_ = 150;
};

} // namespace bro::layout
