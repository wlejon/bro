#pragma once

#include "layout/box.h"
#include "layout/control_text.h"
#include "layout/key_handle_result.h"
#include "layout/text_undo.h"
#include "css/cascade.h"
#include "render/renderer.h"
#include <string>

namespace bro::dom { class Element; }

namespace bro::layout {

// Standalone input control renderer.
// Reads attributes from bro::dom::Element, draws using Renderer.
class ElInput {
public:
    explicit ElInput(render::Renderer* renderer);

    enum class InputType {
        Text, Password, Button, Submit, Reset,
        Checkbox, Radio, Range, Number, Color, Hidden,
        Email, Tel, Url, Search
    };

    InputType inputType(dom::Element* elem) const;
    bool isTextType(dom::Element* elem) const;
    bool isButtonType(dom::Element* elem) const;

    // Draw the input control at its layout position. docOffsetX/Y: the draw
    // pass's document→surface translation (see DrawTraversal::rootOffsetX_) —
    // applied to the absoluteContentBox() projection so lastDrawPos() lands
    // in the pass's surface space: app *content space* for the app document
    // (the engine translates mouse input into it once at the input boundary),
    // window space for system panels.
    void draw(render::Renderer* renderer,
              const htmlayout::layout::LayoutBox& box,
              const htmlayout::css::ComputedStyle& style,
              float offsetX, float offsetY,
              float docOffsetX = 0, float docOffsetY = 0);

    // Focus/cursor state. The cursor is the moving end of the selection, so
    // setting it collapses any selection onto that point.
    int cursorPos() const { return sel_.caret; }
    void setCursorPos(int pos) { sel_.collapseTo(pos); }

    // Selection range (HTMLInputElement.selectionStart / selectionEnd).
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
    // any in-progress composition state — the script owns the value now, so
    // the preedit's recorded position is meaningless.
    void clearHistory() { undo_.clear(); comp_ = {}; }

    // --- IME composition (see TextComposition in text_undo.h) -------------
    bool isComposing() const { return comp_.active; }
    const std::string& compositionText() const { return comp_.preedit; }
    // Replace the current preedit with `text`, starting the composition on
    // the first call (which also deletes any active selection — part of the
    // same eventual undo entry). `cursorCp` is SDL's composition cursor in
    // UTF-8 characters within `text` (< 0 → end); the control caret lands
    // there so the caret renders at the composition cursor. No undo entry.
    KeyHandleResult compositionUpdate(dom::Element* el, const std::string& text,
                                      int cursorCp);
    // Finalize the composition: replace the preedit with `text`, caret after
    // it, and record ONE discrete undo entry from the pre-composition state.
    KeyHandleResult compositionCommit(dom::Element* el, const std::string& text);
    // Abort the composition: restore the pre-composition value and selection.
    // Leaves no undo entry.
    KeyHandleResult compositionCancel(dom::Element* el);

    // Caret index (byte offset into the value) for a point given in the same
    // space as lastDrawPos() — the draw pass's surface space, which is what the
    // engine hands to focusNewControl. A click past the end of the text lands
    // at the end. Text types only; other types return the current caret.
    //
    // Resolves the control's box live rather than reusing lastDrawPos_, so a
    // click that arrives before the first frame (or after a relayout that
    // hasn't repainted yet) still lands on the right character.
    int caretIndexFromPoint(float px, float py);

    // Mouse selection. A press collapses the caret at the point and pins the
    // anchor there (`extend` = false); dragging, or a shift-click, moves only
    // the caret end (`extend` = true), leaving the anchor where the press put it.
    void caretToPoint(float px, float py, bool extend);
    // Double-click: take the word under the point. Triple-click: selectAll().
    void selectWordAtPoint(float px, float py);

    // Owning element (set during attachment)
    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    // Range slider state
    bool isDragging() const { return dragging_; }
    void setDragging(bool d) { dragging_ = d; }

    float rangeMin() const;
    float rangeMax() const;
    float rangeStep() const;
    float rangeValue() const;
    void setRangeValue(float v);

    struct DrawPos { float x, y, w, h; };
    DrawPos lastDrawPos() const { return lastDrawPos_; }

    // Caret rectangle in the draw pass's surface space (content space for the
    // app document), computed live — feeds SDL_SetTextInputArea so the native
    // IME candidate window tracks the caret. False for non-text types or when
    // the control has no box yet.
    bool caretRect(float& x, float& y, float& w, float& h);

    // Chrome sizes for range inputs derived from the element's drawn height,
    // so the thumb always fits inside the element's hit box regardless of
    // how CSS sizes the control.
    static float rangeThumbRadius(float h);
    static float rangeTrackHeight(float h);

    // Width of the spin buttons a number input draws over its right edge.
    static constexpr float kSpinButtonWidth = 16.0f;

    // Key/text input handling — returns result for engine to dispatch events
    KeyHandleResult handleKeyDown(dom::Element* el, int keycode, int mod);
    KeyHandleResult handleTextInput(dom::Element* el, const std::string& text);
    // Paste insertion: same mutation as handleTextInput, but records a
    // discrete (non-coalescing) history entry and reports "insertFromPaste".
    KeyHandleResult pasteText(dom::Element* el, const std::string& text);

    // Content size for layout (intrinsic sizing)
    void getContentSize(float& w, float& h, float maxWidth);

private:
    // The text actually painted for the value — the value itself, except for
    // password types, which render one '*' per byte. Hit-testing and caret
    // placement must measure what was drawn, not the underlying value.
    std::string displayText_() const;
    // Content width available to the text: the box, less the number spinner.
    float textWidth_(float w) const;
    // The control's content box in the draw pass's surface space, computed now.
    DrawPos contentBox_() const;

    void drawText_(float x, float y, float w, float h);
    void drawCheckbox_(float x, float y, float w, float h);
    void drawRadio_(float x, float y, float w, float h);
    void drawRange_(float x, float y, float w, float h);
    void drawColor_(float x, float y, float w, float h);

    std::string getAttr(const std::string& name) const;
    render::FontRef getFontRef() const;
    bool darkScheme_() const;
    bromath::Color accentColor_() const;

    // Delete the selected range from `val` in place and collapse the caret to
    // where it was. No-op (returns false) when the selection is collapsed.
    bool deleteSelection_(std::string& val, std::string& removed);

    // Shared body of handleTextInput / pasteText.
    KeyHandleResult insertText_(dom::Element* el, const std::string& text,
                                bool fromPaste);

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    TextRange sel_;
    // Per-element undo/redo history for the text-editing types.
    TextUndoStack undo_;
    // In-progress IME composition (inactive when comp_.active is false).
    TextComposition comp_;
    // Horizontal scroll of the text under the (fixed) content box, in px. Set
    // in draw() to keep the caret inside the box once the value outgrows it;
    // caretIndexFromPoint adds it back to undo the shift.
    float scrollX_ = 0.0f;
    bool focused_ = false;
    bool dragging_ = false;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
    // The draw pass's document→surface translation, captured in draw(). Zero
    // before the first frame, which is also the correct answer then (nothing
    // has scrolled yet), so contentBox_() works from the very first click.
    float docOffsetX_ = 0.0f;
    float docOffsetY_ = 0.0f;
};

} // namespace bro::layout
