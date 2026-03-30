#pragma once

#include <litehtml.h>

namespace bro::render { class Renderer; }

namespace bro::layout {

/// Custom litehtml element for <input> — acts as a replaced element that
/// renders the value attribute as text inside the input box.
/// litehtml handles box model (width/height/display/borders/background)
/// while we render the value text ourselves.
class ElInput : public litehtml::html_tag {
public:
    ElInput(const std::shared_ptr<litehtml::document>& doc,
            render::Renderer* renderer);

    bool is_replaced() const override { return true; }
    void get_content_size(litehtml::size& sz, litehtml::pixel_t max_width) override;
    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position* clip,
              const std::shared_ptr<litehtml::render_item>& ri) override;
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item>& parent_ri) override;

private:
    render::Renderer* renderer_;
};

} // namespace bro::layout
