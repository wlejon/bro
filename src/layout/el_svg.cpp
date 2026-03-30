#include "layout/el_svg.h"
#include "svg/svg_parser.h"
#include "svg/svg_renderer.h"
#include "render/renderer.h"

#include <litehtml/render_image.h>
#include <cstdlib>

namespace bro::layout {

ElSvg::ElSvg(const std::shared_ptr<litehtml::document>& doc,
               render::Renderer* renderer)
    : html_tag(doc), renderer_(renderer)
{
    m_css.set_display(litehtml::display_inline_block);
}

void ElSvg::parse_attributes() {
    // Map width/height attributes to CSS dimensions (like el_image)
    const char* w = get_attr("width");
    if (w) {
        intrinsicWidth_ = std::strtof(w, nullptr);
        map_to_dimension_property(litehtml::_width_, w);
    }
    const char* h = get_attr("height");
    if (h) {
        intrinsicHeight_ = std::strtof(h, nullptr);
        map_to_dimension_property(litehtml::_height_, h);
    }
}

void ElSvg::compute_styles(bool recursive) {
    html_tag::compute_styles(recursive);
}

void ElSvg::get_content_size(litehtml::size& sz, litehtml::pixel_t /*max_width*/) {
    sz.width = static_cast<litehtml::pixel_t>(intrinsicWidth_);
    sz.height = static_cast<litehtml::pixel_t>(intrinsicHeight_);
}

void ElSvg::draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                  const litehtml::position* clip,
                  const std::shared_ptr<litehtml::render_item>& ri) {
    // Draw backgrounds/borders first
    html_tag::draw(hdc, x, y, clip, ri);

    auto pos = ri->pos();
    pos.x += x;
    pos.y += y;

    if (!clip || pos.does_intersect(clip)) {
        if (pos.width > 0 && pos.height > 0 && renderer_) {
            // Parse SVG tree from our litehtml children
            auto svgRoot = svg::parseSvgTree(shared_from_this());
            // Render into the positioned box
            svg::renderSvg(renderer_,
                           svgRoot,
                           static_cast<float>(pos.x),
                           static_cast<float>(pos.y),
                           static_cast<float>(pos.width),
                           static_cast<float>(pos.height));
        }
    }
}

std::shared_ptr<litehtml::render_item> ElSvg::create_render_item(
    const std::shared_ptr<litehtml::render_item>& parent_ri) {
    // Use render_item_image — it handles replaced element sizing correctly
    auto ret = std::make_shared<litehtml::render_item_image>(shared_from_this());
    ret->parent(parent_ri);
    return ret;
}

} // namespace bro::layout
