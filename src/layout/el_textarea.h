#pragma once

#include "layout/box.h"
#include "css/cascade.h"
#include "render/renderer.h"
#include <string>

namespace bro::dom { class Element; }

namespace bro::layout {

// Standalone textarea control renderer.
class ElTextarea {
public:
    explicit ElTextarea(render::Renderer* renderer);

    void draw(render::Renderer* renderer,
              const htmlayout::layout::LayoutBox& box,
              const htmlayout::css::ComputedStyle& style,
              float offsetX, float offsetY);

    int cursorPos() const { return cursorPos_; }
    void setCursorPos(int pos) { cursorPos_ = pos; }
    bool isFocused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    float scrollY() const { return scrollY_; }
    void setScrollY(float y) { scrollY_ = y; }

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    int rows() const;
    int cols() const;

    void getContentSize(float& w, float& h);

private:
    uint64_t getFontHandle() const;
    std::string getAttr(const std::string& name) const;

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    int cursorPos_ = 0;
    bool focused_ = false;
    float scrollY_ = 0.0f;
    mutable uint64_t cachedFontHandle_ = 0;
};

} // namespace bro::layout
