#pragma once

#include "layout/box.h"
#include "layout/control_text.h"
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

    // The cursor is the moving end of the selection, so setting it collapses
    // any selection onto that point.
    int cursorPos() const { return sel_.caret; }
    void setCursorPos(int pos) { sel_.collapseTo(pos); }

    int selectionStart() const { return sel_.start(); }
    int selectionEnd() const { return sel_.end(); }
    bool hasSelection() const { return !sel_.collapsed(); }
    // Offsets are bytes. A caller naming one inside a multi-byte character (JS
    // counts UTF-16 units, so `value.length` routinely does) is snapped onto a
    // character boundary: a range grows outward to whole characters, a caret
    // settles on the boundary at or before it.
    void setSelectionRange(int start, int end);
    void selectAll();
    // The selected substring of the value — what a copy/cut takes.
    std::string selectedText() const;
    // Remove the selected range from the value and collapse the caret there.
    // False (and no write) when nothing is selected — a cut with a collapsed
    // caret must not eat a character.
    bool cutSelection(dom::Element* el);

    bool isFocused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    // Caret index (byte offset into the value) for a point in the draw pass's
    // surface space — what the engine hands to focusNewControl. Resolves
    // against the same soft-wrapped visual lines the frame drew, so the caret
    // lands under the cursor even in a wrapped, scrolled textarea.
    int caretIndexFromPoint(float px, float py);

    // Mouse selection. A press collapses the caret at the point and pins the
    // anchor there (`extend` = false); dragging, or a shift-click, moves only
    // the caret end (`extend` = true).
    void caretToPoint(float px, float py, bool extend);
    void selectWordAtPoint(float px, float py);

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
    // CSS accent-color (falls back to the UA blue) — tints the selection wash.
    bromath::Color accentColor_() const;
    // Delete the selected range from `val` in place, collapsing the caret to
    // where it was. No-op (returns false) when the selection is collapsed.
    bool deleteSelection_(std::string& val, std::string& removed);

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    TextRange sel_;
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
