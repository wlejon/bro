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

    // docOffsetX/Y: the draw pass's document→surface translation, used to land
    // lastDrawPos() in the pass's surface space — see ElInput::draw.
    void draw(render::Renderer* renderer,
              const htmlayout::layout::LayoutBox& box,
              const htmlayout::css::ComputedStyle& style,
              float offsetX, float offsetY,
              float docOffsetX = 0, float docOffsetY = 0);

    int cursorPos() const { return cursorPos_; }
    void setCursorPos(int pos) { cursorPos_ = pos; }
    bool isFocused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    // Caret index (byte offset into the value) for a point in the draw pass's
    // surface space — what the engine hands to focusNewControl. Resolves
    // against the same soft-wrapped visual lines the frame drew, so the caret
    // lands under the cursor even in a wrapped, scrolled textarea.
    int caretIndexFromPoint(float px, float py);

    struct DrawPos { float x, y, w, h; };
    // Content box in the draw pass's surface space, computed live (not cached
    // from the last frame) so a click before the first paint still hits right.
    DrawPos contentBox() const;

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
    // navigation (up/down/home/end) and click hit-testing need the same wrap
    // width the frame drew with; 0 means "not drawn yet" — nav falls back to
    // hard-newline lines.
    float wrapWidth_ = 0.0f;
    // The draw pass's document→surface translation, captured in draw(). Zero
    // before the first frame, which is the correct answer then (nothing has
    // scrolled yet), so contentBox() works from the very first click.
    float docOffsetX_ = 0.0f;
    float docOffsetY_ = 0.0f;
};

} // namespace bro::layout
