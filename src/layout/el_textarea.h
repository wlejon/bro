#pragma once

#include <litehtml.h>
#include <string>

namespace bro::render { class Renderer; }

namespace bro::layout {

/// Custom litehtml element for <textarea> — multi-line text editing.
/// Acts as a replaced element that renders text content with line wrapping,
/// cursor navigation, and vertical scrolling.
class ElTextarea : public litehtml::html_tag {
public:
    ElTextarea(const std::shared_ptr<litehtml::document>& doc,
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

    // Scroll offset (in pixels, managed by engine or draw)
    float scrollY() const { return scrollY_; }
    void setScrollY(float y) { scrollY_ = y; }

    // Get rows/cols from attributes (with defaults)
    int rows() const;
    int cols() const;

private:
    render::Renderer* renderer_;
    int cursorPos_ = 0;
    bool focused_ = false;
    float scrollY_ = 0.0f;
};

} // namespace bro::layout
