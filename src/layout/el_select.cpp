#include "layout/el_select.h"
#include "render/renderer.h"

#include <litehtml/render_image.h>
#include <algorithm>
#include <cstring>

namespace bro::layout {

ElSelect::ElSelect(const std::shared_ptr<litehtml::document>& doc,
                   render::Renderer* renderer)
    : html_tag(doc), renderer_(renderer)
{
    m_css.set_display(litehtml::display_inline_block);
}

std::vector<ElSelect::Option> ElSelect::getOptions() const {
    std::vector<Option> opts;
    // Walk litehtml children looking for <option> elements
    for (auto& child : m_children) {
        if (!child) continue;

        // Check if this is an <option> tag
        const char* tag = child->get_tagName();
        if (!tag || strcmp(tag, "option") != 0) continue;

        Option opt;
        const char* val = child->get_attr("value");
        opt.value = val ? val : "";

        // Get text content via litehtml's get_text (recurses into text nodes)
        litehtml::string textContent;
        child->get_text(textContent);

        // Trim whitespace
        while (!textContent.empty() && (textContent.front() == ' ' || textContent.front() == '\n' || textContent.front() == '\t'))
            textContent.erase(textContent.begin());
        while (!textContent.empty() && (textContent.back() == ' ' || textContent.back() == '\n' || textContent.back() == '\t'))
            textContent.pop_back();

        opt.text = textContent;

        // If value attribute not set, use text content
        if (opt.value.empty()) {
            opt.value = opt.text;
        }

        opts.push_back(std::move(opt));
    }
    return opts;
}

void ElSelect::parse_attributes() {
    html_tag::parse_attributes();
    initSelectedIndex();
}

void ElSelect::initSelectedIndex() {
    int idx = 0;
    for (auto& child : m_children) {
        if (!child) continue;
        const char* tag = child->get_tagName();
        if (!tag || strcmp(tag, "option") != 0) continue;
        if (child->get_attr("selected")) {
            selectedIndex_ = idx;
            return;
        }
        ++idx;
    }
}

void ElSelect::compute_styles(bool recursive) {
    html_tag::compute_styles(recursive);

    // Set explicit CSS height so render_item_image doesn't compute from aspect ratio.
    if (m_css.get_height().is_predefined()) {
        auto fm = css().get_font_metrics();
        if (fm.height > 0) {
            int h = fm.height + 4;
            litehtml::css_length height;
            height.set_value(static_cast<float>(h), litehtml::css_units_px);
            m_css.set_height(height);
        }
    }
}

void ElSelect::get_content_size(litehtml::size& sz, litehtml::pixel_t /*max_width*/) {
    auto font = css().get_font();
    if (font && renderer_) {
        uint64_t fontHandle = static_cast<uint64_t>(font);
        auto fm = css().get_font_metrics();

        // Measure widest option to size the select
        auto opts = getOptions();
        float maxW = 50.0f; // minimum width
        for (auto& opt : opts) {
            if (!opt.text.empty()) {
                auto tm = renderer_->measureText(opt.text, fontHandle);
                maxW = std::max(maxW, tm.width);
            }
        }

        sz.width = static_cast<litehtml::pixel_t>(maxW) + 28; // padding + arrow space
        sz.height = static_cast<litehtml::pixel_t>(fm.height) + 4;
    } else {
        sz.width = 120;
        sz.height = 20;
    }
}

void ElSelect::draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                    const litehtml::position* clip,
                    const std::shared_ptr<litehtml::render_item>& ri) {
    html_tag::draw(hdc, x, y, clip, ri);

    if (!renderer_) return;

    auto pos = ri->pos();
    pos.x += x;
    pos.y += y;

    if (clip && !pos.does_intersect(clip)) return;
    if (pos.width <= 0 || pos.height <= 0) return;

    // Store position for hit testing
    lastDrawPos_ = {static_cast<float>(pos.x), static_cast<float>(pos.y),
                    static_cast<float>(pos.width), static_cast<float>(pos.height)};

    auto font = css().get_font();
    if (!font) return;
    uint64_t fontHandle = static_cast<uint64_t>(font);

    auto fm = css().get_font_metrics();
    float lineHeight = static_cast<float>(fm.height);
    float ascent = static_cast<float>(fm.ascent);
    float height = static_cast<float>(pos.height);

    auto textColor = css().get_color();
    render::Color color = {textColor.red, textColor.green, textColor.blue, textColor.alpha};

    float padX = 4.0f;
    float textY = static_cast<float>(pos.y) + (height - lineHeight) / 2.0f + ascent;

    // Clip to select bounds
    renderer_->save();
    renderer_->setClip(static_cast<float>(pos.x), static_cast<float>(pos.y),
                       static_cast<float>(pos.width), static_cast<float>(pos.height));

    // Draw selected option text
    auto opts = getOptions();
    int idx = std::clamp(selectedIndex_, 0, std::max(0, static_cast<int>(opts.size()) - 1));
    if (!opts.empty() && idx < static_cast<int>(opts.size())) {
        renderer_->drawText(opts[idx].text,
                           static_cast<float>(pos.x) + padX, textY,
                           fontHandle, color);
    }

    // Draw dropdown arrow (small triangle on the right)
    float arrowX = static_cast<float>(pos.x + pos.width) - 16.0f;
    float arrowY = static_cast<float>(pos.y) + height / 2.0f;
    render::PointF arrowPts[3] = {
        {arrowX, arrowY - 3.0f},
        {arrowX + 8.0f, arrowY - 3.0f},
        {arrowX + 4.0f, arrowY + 3.0f}
    };
    renderer_->drawPolygon(std::span<const render::PointF>(arrowPts, 3),
                          color, {0, 0, 0, 0}, 0.0f);

    renderer_->restore();

    // Draw dropdown list when open
    if (open_ && !opts.empty()) {
        float dropX = static_cast<float>(pos.x);
        float dropY = static_cast<float>(pos.y + pos.height);
        float dropW = static_cast<float>(pos.width);
        float dropH = lineHeight * static_cast<float>(opts.size()) + 4.0f;

        // Background
        renderer_->fillRect(dropX, dropY, dropW, dropH, {255, 255, 255, 255});
        // Border
        renderer_->drawRect(dropX, dropY, dropW, dropH, {118, 118, 118, 255});

        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
            float itemY = dropY + 2.0f + i * lineHeight;

            // Highlight
            if (i == highlightedIndex_) {
                renderer_->fillRect(dropX + 1, itemY, dropW - 2, lineHeight,
                                   {0, 120, 215, 255});
                renderer_->drawText(opts[i].text, dropX + padX, itemY + ascent,
                                   fontHandle, {255, 255, 255, 255});
            } else {
                renderer_->drawText(opts[i].text, dropX + padX, itemY + ascent,
                                   fontHandle, color);
            }
        }
    }
}

std::shared_ptr<litehtml::render_item> ElSelect::create_render_item(
    const std::shared_ptr<litehtml::render_item>& parent_ri) {
    auto ret = std::make_shared<litehtml::render_item_image>(shared_from_this());
    ret->parent(parent_ri);
    return ret;
}

} // namespace bro::layout
