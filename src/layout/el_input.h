#pragma once

#include <litehtml.h>
#include <string>

namespace bro::render { class Renderer; }

namespace bro::layout {

/// Custom litehtml element for <input> — acts as a replaced element that
/// renders differently based on the type attribute.
/// Supported types: text, password, button, submit, reset, checkbox, radio,
/// range, number, color, hidden, email, tel, url, search.
class ElInput : public litehtml::html_tag {
public:
    ElInput(const std::shared_ptr<litehtml::document>& doc,
            render::Renderer* renderer);

    bool is_replaced() const override { return true; }
    void compute_styles(bool recursive) override;
    void get_content_size(litehtml::size& sz, litehtml::pixel_t max_width) override;
    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position* clip,
              const std::shared_ptr<litehtml::render_item>& ri) override;
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item>& parent_ri) override;

    // Focus/cursor state (managed by engine)
    int cursorPos() const { return cursorPos_; }
    void setCursorPos(int pos) { cursorPos_ = pos; }
    bool isFocused() const { return focused_; }
    void setFocused(bool f) { focused_ = f; }

    // Type query helpers
    enum class InputType {
        Text, Password, Button, Submit, Reset,
        Checkbox, Radio, Range, Number, Color, Hidden,
        // Text aliases
        Email, Tel, Url, Search
    };
    InputType inputType() const;
    bool isTextType() const;
    bool isButtonType() const;

    // Range slider state
    bool isDragging() const { return dragging_; }
    void setDragging(bool d) { dragging_ = d; }

    // Range helpers
    float rangeMin() const;
    float rangeMax() const;
    float rangeStep() const;
    float rangeValue() const;
    void setRangeValue(float v);

    // Store last drawn position for range hit testing
    struct DrawPos { float x, y, w, h; };
    DrawPos lastDrawPos() const { return lastDrawPos_; }

    // Color picker state
    bool isPickerOpen() const { return pickerOpen_; }
    void setPickerOpen(bool o) { pickerOpen_ = o; }
    void drawColorPicker();

private:
    void drawText_(const litehtml::position& pos);
    void drawCheckbox_(const litehtml::position& pos);
    void drawRadio_(const litehtml::position& pos);
    void drawRange_(const litehtml::position& pos);
    void drawNumber_(const litehtml::position& pos);
    void drawColor_(const litehtml::position& pos);

    render::Renderer* renderer_;
    int cursorPos_ = 0;
    bool focused_ = false;
    bool dragging_ = false;
    bool pickerOpen_ = false;
    mutable DrawPos lastDrawPos_ = {0, 0, 0, 0};
};

} // namespace bro::layout
