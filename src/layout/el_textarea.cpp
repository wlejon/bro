#include "layout/el_textarea.h"
#include "render/renderer.h"

#include <litehtml/render_image.h>
#include <algorithm>
#include <cstring>

namespace bro::layout {

ElTextarea::ElTextarea(const std::shared_ptr<litehtml::document>& doc,
                       render::Renderer* renderer)
    : html_tag(doc), renderer_(renderer)
{
    m_css.set_display(litehtml::display_inline_block);
}

void ElTextarea::compute_styles(bool recursive) {
    html_tag::compute_styles(recursive);

    // Set explicit CSS height from rows attribute so render_item_image
    // doesn't compute it from aspect ratio (which is wrong for form controls).
    if (m_css.get_height().is_predefined()) {
        auto fm = css().get_font_metrics();
        if (fm.height > 0) {
            int h = fm.height * rows() + 4;
            litehtml::css_length height;
            height.set_value(static_cast<float>(h), litehtml::css_units_px);
            m_css.set_height(height);
        }
    }

    // Similarly for width if not set
    if (m_css.get_width().is_predefined()) {
        auto font = css().get_font();
        if (font && renderer_) {
            auto tm = renderer_->measureText("M", static_cast<uint64_t>(font));
            int w = static_cast<int>(tm.width * cols()) + 8;
            litehtml::css_length width;
            width.set_value(static_cast<float>(w), litehtml::css_units_px);
            m_css.set_width(width);
        }
    }
}

int ElTextarea::rows() const {
    const char* r = get_attr("rows");
    if (r) {
        int v = atoi(r);
        if (v > 0) return v;
    }
    return 2; // HTML default
}

int ElTextarea::cols() const {
    const char* c = get_attr("cols");
    if (c) {
        int v = atoi(c);
        if (v > 0) return v;
    }
    return 20; // HTML default
}

void ElTextarea::get_content_size(litehtml::size& sz, litehtml::pixel_t /*max_width*/) {
    // Size based on rows/cols attributes, using average character width
    auto font = css().get_font();
    if (font && renderer_) {
        uint64_t fontHandle = static_cast<uint64_t>(font);
        auto tm = renderer_->measureText("M", fontHandle);
        float charW = tm.width;
        auto fm = css().get_font_metrics();
        float lineH = static_cast<float>(fm.height);

        sz.width = static_cast<litehtml::pixel_t>(charW * cols()) + 8; // +padding
        sz.height = static_cast<litehtml::pixel_t>(lineH * rows()) + 4;
    } else {
        sz.width = 173;
        sz.height = 40;
    }
}

// Split text into lines by newline characters
static std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
        // If text ends with newline, add empty trailing line
        if (start == text.size()) {
            lines.push_back("");
        }
    }
    if (lines.empty()) lines.push_back("");
    return lines;
}

// Find which line and column a cursor position maps to
static std::pair<int, int> posToLineCol(const std::string& text, int pos) {
    int line = 0, col = 0;
    for (int i = 0; i < pos && i < static_cast<int>(text.size()); ++i) {
        if (text[i] == '\n') {
            ++line;
            col = 0;
        } else {
            ++col;
        }
    }
    return {line, col};
}

// Find cursor position from line and column
static int lineColToPos(const std::vector<std::string>& lines, int line, int col) {
    int pos = 0;
    for (int i = 0; i < line && i < static_cast<int>(lines.size()); ++i) {
        pos += static_cast<int>(lines[i].size()) + 1; // +1 for newline
    }
    if (line < static_cast<int>(lines.size())) {
        pos += std::min(col, static_cast<int>(lines[line].size()));
    }
    return pos;
}

void ElTextarea::draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                      const litehtml::position* clip,
                      const std::shared_ptr<litehtml::render_item>& ri) {
    html_tag::draw(hdc, x, y, clip, ri);

    if (!renderer_) return;

    auto pos = ri->pos();
    pos.x += x;
    pos.y += y;

    if (clip && !pos.does_intersect(clip)) return;
    if (pos.width <= 0 || pos.height <= 0) return;

    // Get text value
    const char* val = get_attr("value");
    const char* placeholder = get_attr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (val && *val) {
        text = val;
    } else if (placeholder && *placeholder) {
        text = placeholder;
        isPlaceholder = true;
    }

    // Get font
    auto font = css().get_font();
    if (!font) return;
    uint64_t fontHandle = static_cast<uint64_t>(font);

    auto fm = css().get_font_metrics();
    float lineHeight = static_cast<float>(fm.height);
    float ascent = static_cast<float>(fm.ascent);

    float padX = 4.0f;
    float padY = 2.0f;
    float contentW = static_cast<float>(pos.width) - padX * 2;
    float contentH = static_cast<float>(pos.height) - padY * 2;

    // Split into lines
    auto lines = splitLines(text);

    // Auto-scroll to keep cursor visible when focused
    if (focused_) {
        auto [cursorLine, cursorCol] = posToLineCol(text, std::clamp(cursorPos_, 0, static_cast<int>(text.size())));
        float cursorY = cursorLine * lineHeight;

        if (cursorY < scrollY_) {
            scrollY_ = cursorY;
        } else if (cursorY + lineHeight > scrollY_ + contentH) {
            scrollY_ = cursorY + lineHeight - contentH;
        }

        float maxScroll = std::max(0.0f, static_cast<float>(lines.size()) * lineHeight - contentH);
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
    }

    // Clip to textarea bounds
    renderer_->save();
    renderer_->setClip(static_cast<float>(pos.x), static_cast<float>(pos.y),
                       static_cast<float>(pos.width), static_cast<float>(pos.height));

    // Determine text color
    render::Color color;
    if (isPlaceholder) {
        color = {128, 128, 128, 180};
    } else {
        auto c = css().get_color();
        color = {c.red, c.green, c.blue, c.alpha};
    }

    // Draw lines
    float baseX = static_cast<float>(pos.x) + padX;
    float baseY = static_cast<float>(pos.y) + padY - scrollY_;

    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        float lineY = baseY + i * lineHeight;
        // Skip lines outside visible area
        if (lineY + lineHeight < static_cast<float>(pos.y)) continue;
        if (lineY > static_cast<float>(pos.y + pos.height)) break;

        if (!lines[i].empty()) {
            renderer_->drawText(lines[i], baseX, lineY + ascent, fontHandle, color);
        }
    }

    // Draw cursor when focused
    if (focused_ && !isPlaceholder) {
        std::string valStr = text;
        int cpos = std::clamp(cursorPos_, 0, static_cast<int>(valStr.size()));
        auto [cursorLine, cursorCol] = posToLineCol(valStr, cpos);

        float cursorX = baseX;
        if (cursorCol > 0 && cursorLine < static_cast<int>(lines.size())) {
            std::string beforeCursor = lines[cursorLine].substr(0, cursorCol);
            auto tm = renderer_->measureText(beforeCursor, fontHandle);
            cursorX += tm.width;
        }

        float cursorTop = baseY + cursorLine * lineHeight;
        float cursorBottom = cursorTop + lineHeight;

        auto c = css().get_color();
        render::Color cursorColor = {c.red, c.green, c.blue, c.alpha};
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, cursorColor, 1.0f);
    }

    renderer_->restore();
}

std::shared_ptr<litehtml::render_item> ElTextarea::create_render_item(
    const std::shared_ptr<litehtml::render_item>& parent_ri) {
    auto ret = std::make_shared<litehtml::render_item_image>(shared_from_this());
    ret->parent(parent_ri);
    return ret;
}

} // namespace bro::layout
