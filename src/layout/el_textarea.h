#pragma once

#include "layout/box.h"
#include "layout/control_text.h"
#include "layout/key_handle_result.h"
#include "layout/text_undo.h"
#include "layout/value_change.h"
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
    // Offsets are bytes into the UTF-8 value (the JS binding converts to/from
    // UTF-16 code units at its boundary). A caller naming one inside a
    // multi-byte character is snapped onto a character boundary: a range grows
    // outward to whole characters, a caret settles on the boundary at or
    // before it.
    void setSelectionRange(int start, int end);
    void selectAll();
    // The selected substring of the value — what a copy/cut takes.
    std::string selectedText() const;
    // Remove the selected range from the value and collapse the caret there.
    // False (and no write) when nothing is selected — a cut with a collapsed
    // caret must not eat a character.
    bool cutSelection(dom::Element* el);

    bool isFocused() const { return focused_; }
    // Losing focus ends any coalescing run — refocusing and typing starts a
    // fresh undo entry, as in a browser.
    void setFocused(bool f) {
        if (!f && focused_) undo_.breakCoalescing();
        focused_ = f;
    }

    // Drop the undo/redo history. Called on programmatic `.value =` writes,
    // which invalidate every recorded delta (browser behavior). Also drops
    // any in-progress composition state — the script owns the value now.
    void clearHistory() { undo_.clear(); comp_ = {}; }

    // --- The `change` event (see layout/value_change.h) --------------------
    // Only the departure, unlike ElInput's: Enter in a textarea is a newline,
    // so there is no key here that means "I mean it".
    /// This element's value counts as already reported.
    void armChange(dom::Element* el);
    /// Is there an edit to report on this element? True once per edit.
    bool takeChange(dom::Element* el);

    // --- IME composition (see TextComposition in text_undo.h; semantics
    // identical to ElInput's) ----------------------------------------------
    bool isComposing() const { return comp_.active; }
    const std::string& compositionText() const { return comp_.preedit; }
    KeyHandleResult compositionUpdate(dom::Element* el, const std::string& text,
                                      int cursorCp);
    KeyHandleResult compositionCommit(dom::Element* el, const std::string& text);
    KeyHandleResult compositionCancel(dom::Element* el);

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

    // Caret rectangle in the draw pass's surface space, computed live against
    // the same soft-wrapped visual lines the frame drew — feeds
    // SDL_SetTextInputArea so the IME candidate window tracks the caret.
    bool caretRect(float& x, float& y, float& w, float& h);

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    int rows() const;
    int cols() const;

    // Key/text input handling — returns result for engine to dispatch events
    KeyHandleResult handleKeyDown(dom::Element* el, int keycode, int mod);
    KeyHandleResult handleTextInput(dom::Element* el, const std::string& text);
    // Paste insertion: same mutation as handleTextInput, but records a
    // discrete (non-coalescing) history entry and reports "insertFromPaste".
    KeyHandleResult pasteText(dom::Element* el, const std::string& text);

    void getContentSize(float& w, float& h);

private:
    // One arrow-key step of the caret through `val`, by shaped cluster rather
    // than by character, so the caret never stops inside a ligature or a
    // combining sequence — a place with no geometry to draw it at. Falls back
    // to one character where there is no shaper. Steps within one hard line;
    // clusters never span a newline.
    int caretStepPrev_(const std::string& val, int pos) const;
    int caretStepNext_(const std::string& val, int pos) const;

    render::FontRef getFontRef() const;
    std::string getAttr(const std::string& name) const;
    // CSS accent-color (falls back to the UA blue) — tints the selection wash.
    bromath::Color accentColor_() const;
    // Delete the selected range from `val` in place, collapsing the caret to
    // where it was. No-op (returns false) when the selection is collapsed.
    bool deleteSelection_(std::string& val, std::string& removed);

    // Shared body of handleTextInput / pasteText.
    KeyHandleResult insertText_(dom::Element* el, const std::string& text,
                                bool fromPaste);

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    TextRange sel_;
    // Per-element undo/redo history.
    TextUndoStack undo_;
    // In-progress IME composition (inactive when comp_.active is false).
    TextComposition comp_;
    // The value this control last reported through a `change` event.
    ValueChange change_;
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
