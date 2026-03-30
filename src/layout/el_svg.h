#pragma once

#include <litehtml.h>

namespace bro::render { class Renderer; }

namespace bro::layout {

/// Custom litehtml element for <svg> — acts as a replaced element.
/// litehtml handles the box model (width/height/display) while we
/// render the SVG content ourselves using the SVG parser + renderer.
class ElSvg : public litehtml::html_tag {
public:
    ElSvg(const std::shared_ptr<litehtml::document>& doc,
           render::Renderer* renderer);

    bool is_replaced() const override { return true; }
    void get_content_size(litehtml::size& sz, litehtml::pixel_t max_width) override;
    void parse_attributes() override;
    void compute_styles(bool recursive) override;
    void draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
              const litehtml::position* clip,
              const std::shared_ptr<litehtml::render_item>& ri) override;
    std::shared_ptr<litehtml::render_item> create_render_item(
        const std::shared_ptr<litehtml::render_item>& parent_ri) override;

private:
    render::Renderer* renderer_;
    float intrinsicWidth_ = 300;
    float intrinsicHeight_ = 150;
};

} // namespace bro::layout
