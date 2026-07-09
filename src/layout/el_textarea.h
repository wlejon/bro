#pragma once

#include "layout/box.h"
#include "layout/key_handle_result.h"
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

    // Key/text input handling — returns result for engine to dispatch events
    KeyHandleResult handleKeyDown(dom::Element* el, int keycode, int mod);
    KeyHandleResult handleTextInput(dom::Element* el, const std::string& text);

    void getContentSize(float& w, float& h);

private:
    render::FontRef getFontRef() const;
    std::string getAttr(const std::string& name) const;

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    int cursorPos_ = 0;
    bool focused_ = false;
    float scrollY_ = 0.0f;
    // Content width the text last soft-wrapped against (set in draw()). Cursor
    // navigation (up/down/home/end) needs the same wrap width the frame drew
    // with; 0 means "not drawn yet" — nav falls back to hard-newline lines.
    float wrapWidth_ = 0.0f;
};

} // namespace bro::layout
