#include "layout/el_input.h"
#include "render/renderer.h"

#include <litehtml/render_image.h>

namespace bro::layout {

ElInput::ElInput(const std::shared_ptr<litehtml::document>& doc,
                 render::Renderer* renderer)
    : html_tag(doc), renderer_(renderer)
{
    m_css.set_display(litehtml::display_inline_block);
}

void ElInput::get_content_size(litehtml::size& sz, litehtml::pixel_t max_width) {
    // Default intrinsic size similar to browser defaults for <input>
    // type="text" is typically ~173px wide, ~20px tall
    const char* type = get_attr("type");
    if (type && (strcmp(type, "button") == 0 || strcmp(type, "submit") == 0 || strcmp(type, "reset") == 0)) {
        // Button-type inputs: size to text
        const char* val = get_attr("value");
        std::string text = val ? val : (type ? type : "");
        if (!text.empty() && renderer_) {
            // Use a default font for measurement
            auto font = css().get_font();
            if (font) {
                auto tm = renderer_->measureText(text, static_cast<uint64_t>(font));
                sz.width = static_cast<litehtml::pixel_t>(tm.width) + 16; // padding
                sz.height = static_cast<litehtml::pixel_t>(tm.height) + 4;
                return;
            }
        }
        sz.width = 80;
        sz.height = 20;
    } else {
        // Text input: use a standard width
        sz.width = (max_width > 0 && max_width < 200) ? max_width : 173;
        sz.height = 20;
    }
}

void ElInput::draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                   const litehtml::position* clip,
                   const std::shared_ptr<litehtml::render_item>& ri) {
    // Draw backgrounds/borders first (handled by html_tag)
    html_tag::draw(hdc, x, y, clip, ri);

    if (!renderer_) return;

    auto pos = ri->pos();
    pos.x += x;
    pos.y += y;

    if (clip && !pos.does_intersect(clip)) return;
    if (pos.width <= 0 || pos.height <= 0) return;

    // Determine text to display
    const char* val = get_attr("value");
    const char* placeholder = get_attr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (val && *val) {
        // For password inputs, mask the value
        const char* type = get_attr("type");
        if (type && strcmp(type, "password") == 0) {
            text = std::string(strlen(val), '*');
        } else {
            text = val;
        }
    } else if (placeholder && *placeholder) {
        text = placeholder;
        isPlaceholder = true;
    }

    if (text.empty()) return;

    // Get the element's font
    auto font = css().get_font();
    if (!font) return;
    uint64_t fontHandle = static_cast<uint64_t>(font);

    // Use litehtml's font metrics for proper baseline positioning
    auto fm = css().get_font_metrics();
    float fontHeight = static_cast<float>(fm.height);
    float ascent = static_cast<float>(fm.ascent);
    // Center the text vertically in the input box, position at baseline
    float textY = static_cast<float>(pos.y) + (static_cast<float>(pos.height) - fontHeight) / 2.0f + ascent;

    // Clip text to input width with padding
    float padX = 4.0f;
    float drawX = static_cast<float>(pos.x) + padX;

    // Choose color: placeholder is dimmed, value uses element color
    render::Color color;
    if (isPlaceholder) {
        color = {128, 128, 128, 180}; // gray for placeholder
    } else {
        auto c = css().get_color();
        color = {c.red, c.green, c.blue, c.alpha};
    }

    // Clip to input bounds
    renderer_->save();
    renderer_->setClip(static_cast<float>(pos.x), static_cast<float>(pos.y),
                       static_cast<float>(pos.width), static_cast<float>(pos.height));

    renderer_->drawText(text, drawX, textY, fontHandle, color);

    renderer_->restore();
}

std::shared_ptr<litehtml::render_item> ElInput::create_render_item(
    const std::shared_ptr<litehtml::render_item>& parent_ri) {
    auto ret = std::make_shared<litehtml::render_item_image>(shared_from_this());
    ret->parent(parent_ri);
    return ret;
}

} // namespace bro::layout
