#pragma once

#include "layout/box.h"
#include "layout/key_handle_result.h"
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

    // Focus/cursor state
    int cursorPos() const { return cursorPos_; }
    void setCursorPos(int pos) { cursorPos_ = pos; selStart_ = selEnd_ = pos; }

    // Selection range (HTMLInputElement.selectionStart / selectionEnd).
    // Collapsed = both equal cursorPos_. Direction is informational only.
    int selectionStart() const { return selStart_; }
    int selectionEnd() const { return selEnd_; }
    void setSelectionRange(int start, int end) {
        if (start < 0) start = 0;
        if (end < start) end = start;
        selStart_ = start; selEnd_ = end;
        cursorPos_ = end;
    }
    bool isFocused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

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

    // Chrome sizes for range inputs derived from the element's drawn height,
    // so the thumb always fits inside the element's hit box regardless of
    // how CSS sizes the control.
    static float rangeThumbRadius(float h);
    static float rangeTrackHeight(float h);

    // Key/text input handling — returns result for engine to dispatch events
    KeyHandleResult handleKeyDown(dom::Element* el, int keycode, int mod);
    KeyHandleResult handleTextInput(dom::Element* el, const std::string& text);

    // Content size for layout (intrinsic sizing)
    void getContentSize(float& w, float& h, float maxWidth);

private:
    void drawText_(float x, float y, float w, float h);
    void drawCheckbox_(float x, float y, float w, float h);
    void drawRadio_(float x, float y, float w, float h);
    void drawRange_(float x, float y, float w, float h);
    void drawColor_(float x, float y, float w, float h);

    std::string getAttr(const std::string& name) const;
    render::FontRef getFontRef() const;
    bool darkScheme_() const;
    bromath::Color accentColor_() const;

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    int cursorPos_ = 0;
    int selStart_ = 0;
    int selEnd_ = 0;
    bool focused_ = false;
    bool dragging_ = false;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
};

} // namespace bro::layout
