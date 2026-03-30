#include "layout/el_input.h"
#include "render/renderer.h"

#include <litehtml/render_image.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bro::layout {

ElInput::ElInput(const std::shared_ptr<litehtml::document>& doc,
                 render::Renderer* renderer)
    : html_tag(doc), renderer_(renderer)
{
    m_css.set_display(litehtml::display_inline_block);
}

// ---------------------------------------------------------------------------
// Type helpers
// ---------------------------------------------------------------------------

ElInput::InputType ElInput::inputType() const {
    const char* type = get_attr("type");
    if (!type || !*type) return InputType::Text;
    if (strcmp(type, "password") == 0) return InputType::Password;
    if (strcmp(type, "button") == 0)   return InputType::Button;
    if (strcmp(type, "submit") == 0)   return InputType::Submit;
    if (strcmp(type, "reset") == 0)    return InputType::Reset;
    if (strcmp(type, "checkbox") == 0) return InputType::Checkbox;
    if (strcmp(type, "radio") == 0)    return InputType::Radio;
    if (strcmp(type, "range") == 0)    return InputType::Range;
    if (strcmp(type, "number") == 0)   return InputType::Number;
    if (strcmp(type, "color") == 0)    return InputType::Color;
    if (strcmp(type, "hidden") == 0)   return InputType::Hidden;
    if (strcmp(type, "email") == 0)    return InputType::Email;
    if (strcmp(type, "tel") == 0)      return InputType::Tel;
    if (strcmp(type, "url") == 0)      return InputType::Url;
    if (strcmp(type, "search") == 0)   return InputType::Search;
    return InputType::Text;
}

bool ElInput::isTextType() const {
    auto t = inputType();
    return t == InputType::Text || t == InputType::Password ||
           t == InputType::Email || t == InputType::Tel ||
           t == InputType::Url || t == InputType::Search ||
           t == InputType::Number;
}

bool ElInput::isButtonType() const {
    auto t = inputType();
    return t == InputType::Button || t == InputType::Submit || t == InputType::Reset;
}

// ---------------------------------------------------------------------------
// Range helpers
// ---------------------------------------------------------------------------

float ElInput::rangeMin() const {
    const char* a = get_attr("min");
    return a ? static_cast<float>(atof(a)) : 0.0f;
}

float ElInput::rangeMax() const {
    const char* a = get_attr("max");
    return a ? static_cast<float>(atof(a)) : 100.0f;
}

float ElInput::rangeStep() const {
    const char* a = get_attr("step");
    return (a && atof(a) > 0) ? static_cast<float>(atof(a)) : 1.0f;
}

float ElInput::rangeValue() const {
    const char* v = get_attr("value");
    if (v && *v) return static_cast<float>(atof(v));
    return (rangeMin() + rangeMax()) / 2.0f;
}

void ElInput::setRangeValue(float v) {
    float mn = rangeMin(), mx = rangeMax(), st = rangeStep();
    v = std::clamp(v, mn, mx);
    // Snap to step
    if (st > 0) {
        v = mn + std::round((v - mn) / st) * st;
        v = std::clamp(v, mn, mx);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    // We can't call set_attr on html_tag, so we rely on engine to set the DOM attribute
    // This method is just for computing the snapped value — engine sets the attribute
}

// ---------------------------------------------------------------------------
// Sizing
// ---------------------------------------------------------------------------

void ElInput::compute_styles(bool recursive) {
    html_tag::compute_styles(recursive);

    auto t = inputType();

    // Set explicit CSS height for non-text types to prevent aspect-ratio scaling
    if (t == InputType::Checkbox || t == InputType::Radio) {
        if (m_css.get_height().is_predefined()) {
            litehtml::css_length h;
            h.set_value(16.0f, litehtml::css_units_px);
            m_css.set_height(h);
        }
        if (m_css.get_width().is_predefined()) {
            litehtml::css_length w;
            w.set_value(16.0f, litehtml::css_units_px);
            m_css.set_width(w);
        }
    } else if (t == InputType::Range) {
        if (m_css.get_height().is_predefined()) {
            litehtml::css_length h;
            h.set_value(20.0f, litehtml::css_units_px);
            m_css.set_height(h);
        }
    } else if (t == InputType::Color) {
        if (m_css.get_height().is_predefined()) {
            litehtml::css_length h;
            h.set_value(24.0f, litehtml::css_units_px);
            m_css.set_height(h);
        }
        if (m_css.get_width().is_predefined()) {
            litehtml::css_length w;
            w.set_value(44.0f, litehtml::css_units_px);
            m_css.set_width(w);
        }
    } else if (t == InputType::Hidden) {
        m_css.set_display(litehtml::display_none);
    }
}

void ElInput::get_content_size(litehtml::size& sz, litehtml::pixel_t max_width) {
    auto t = inputType();

    if (t == InputType::Hidden) {
        sz.width = 0;
        sz.height = 0;
        return;
    }

    if (t == InputType::Checkbox || t == InputType::Radio) {
        sz.width = 16;
        sz.height = 16;
        return;
    }

    if (t == InputType::Range) {
        sz.width = 160;
        sz.height = 20;
        return;
    }

    if (t == InputType::Color) {
        sz.width = 44;
        sz.height = 24;
        return;
    }

    if (isButtonType()) {
        const char* val = get_attr("value");
        const char* type = get_attr("type");
        std::string text = val ? val : (type ? type : "");
        if (!text.empty() && renderer_) {
            auto font = css().get_font();
            if (font) {
                auto tm = renderer_->measureText(text, static_cast<uint64_t>(font));
                sz.width = static_cast<litehtml::pixel_t>(tm.width) + 16;
                sz.height = static_cast<litehtml::pixel_t>(tm.height) + 4;
                return;
            }
        }
        sz.width = 80;
        sz.height = 20;
        return;
    }

    // Text-like types (text, password, email, tel, url, search, number)
    sz.width = (max_width > 0 && max_width < 200) ? max_width : 173;
    sz.height = 20;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void ElInput::draw(litehtml::uint_ptr hdc, litehtml::pixel_t x, litehtml::pixel_t y,
                   const litehtml::position* clip,
                   const std::shared_ptr<litehtml::render_item>& ri) {
    html_tag::draw(hdc, x, y, clip, ri);

    if (!renderer_) return;

    auto pos = ri->pos();
    pos.x += x;
    pos.y += y;

    if (clip && !pos.does_intersect(clip)) return;
    if (pos.width <= 0 || pos.height <= 0) return;

    lastDrawPos_ = {static_cast<float>(pos.x), static_cast<float>(pos.y),
                    static_cast<float>(pos.width), static_cast<float>(pos.height)};

    auto t = inputType();
    if (t == InputType::Hidden) return;

    switch (t) {
        case InputType::Checkbox: drawCheckbox_(pos); return;
        case InputType::Radio:    drawRadio_(pos); return;
        case InputType::Range:    drawRange_(pos); return;
        case InputType::Color:    drawColor_(pos); return;
        default: break;
    }

    // Text-like and button types
    drawText_(pos);
}

void ElInput::drawText_(const litehtml::position& pos) {
    const char* val = get_attr("value");
    const char* placeholder = get_attr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (val && *val) {
        if (inputType() == InputType::Password) {
            text = std::string(strlen(val), '*');
        } else {
            text = val;
        }
    } else if (!focused_ && placeholder && *placeholder) {
        text = placeholder;
        isPlaceholder = true;
    }

    auto font = css().get_font();
    if (!font) return;
    uint64_t fontHandle = static_cast<uint64_t>(font);

    auto fm = css().get_font_metrics();
    float fontHeight = static_cast<float>(fm.height);
    float ascent = static_cast<float>(fm.ascent);
    float textY = static_cast<float>(pos.y) + (static_cast<float>(pos.height) - fontHeight) / 2.0f + ascent;

    float padX = 4.0f;
    float drawX = static_cast<float>(pos.x) + padX;

    renderer_->save();
    renderer_->setClip(static_cast<float>(pos.x), static_cast<float>(pos.y),
                       static_cast<float>(pos.width), static_cast<float>(pos.height));

    if (!text.empty()) {
        render::Color color;
        if (isPlaceholder) {
            color = {128, 128, 128, 180};
        } else {
            auto c = css().get_color();
            color = {c.red, c.green, c.blue, c.alpha};
        }
        renderer_->drawText(text, drawX, textY, fontHandle, color);
    }

    // Draw cursor when focused (text types only, not buttons)
    if (focused_ && isTextType()) {
        std::string valStr = (val && *val) ? val : "";
        int cpos = std::clamp(cursorPos_, 0, static_cast<int>(valStr.size()));
        std::string beforeCursor = valStr.substr(0, cpos);
        float cursorX = drawX;
        if (!beforeCursor.empty()) {
            auto tm = renderer_->measureText(beforeCursor, fontHandle);
            cursorX += tm.width;
        }

        float cursorTop = static_cast<float>(pos.y) + (static_cast<float>(pos.height) - fontHeight) / 2.0f;
        float cursorBottom = cursorTop + fontHeight;
        auto c = css().get_color();
        render::Color cursorColor = {c.red, c.green, c.blue, c.alpha};
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, cursorColor, 1.0f);
    }

    // Draw spin buttons for number type
    if (inputType() == InputType::Number) {
        float btnW = 16.0f;
        float bx = static_cast<float>(pos.x + pos.width) - btnW;
        float by = static_cast<float>(pos.y);
        float bh = static_cast<float>(pos.height);

        // Divider line
        renderer_->drawLine(bx, by, bx, by + bh, {180, 180, 180, 255}, 1.0f);
        // Midpoint line
        renderer_->drawLine(bx, by + bh / 2, bx + btnW, by + bh / 2, {180, 180, 180, 255}, 1.0f);

        // Up triangle
        float cx = bx + btnW / 2;
        render::PointF upPts[3] = {
            {cx - 4, by + bh / 4 + 2},
            {cx + 4, by + bh / 4 + 2},
            {cx, by + bh / 4 - 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(upPts, 3),
                              {80, 80, 80, 255}, {0, 0, 0, 0}, 0.0f);

        // Down triangle
        render::PointF downPts[3] = {
            {cx - 4, by + bh * 3 / 4 - 2},
            {cx + 4, by + bh * 3 / 4 - 2},
            {cx, by + bh * 3 / 4 + 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(downPts, 3),
                              {80, 80, 80, 255}, {0, 0, 0, 0}, 0.0f);
    }

    renderer_->restore();
}

void ElInput::drawCheckbox_(const litehtml::position& pos) {
    float x = static_cast<float>(pos.x);
    float y = static_cast<float>(pos.y);
    float w = static_cast<float>(pos.width);
    float h = static_cast<float>(pos.height);
    float sz = std::min(w, h);

    // Center the checkbox
    float bx = x + (w - sz) / 2;
    float by = y + (h - sz) / 2;

    // Box background
    renderer_->fillRect(bx, by, sz, sz, {255, 255, 255, 255});
    renderer_->drawRect(bx, by, sz, sz, {118, 118, 118, 255});

    // Focus ring
    if (focused_) {
        renderer_->drawRect(bx - 1, by - 1, sz + 2, sz + 2, {0, 120, 215, 255});
    }

    // Checkmark
    const char* checked = get_attr("checked");
    if (checked) {
        // Draw a checkmark using two lines
        float pad = sz * 0.2f;
        float x1 = bx + pad;
        float y1 = by + sz * 0.5f;
        float x2 = bx + sz * 0.4f;
        float y2 = by + sz - pad;
        float x3 = bx + sz - pad;
        float y3 = by + pad;

        renderer_->drawLine(x1, y1, x2, y2, {0, 0, 0, 255}, 2.0f);
        renderer_->drawLine(x2, y2, x3, y3, {0, 0, 0, 255}, 2.0f);
    }
}

void ElInput::drawRadio_(const litehtml::position& pos) {
    float x = static_cast<float>(pos.x);
    float y = static_cast<float>(pos.y);
    float w = static_cast<float>(pos.width);
    float h = static_cast<float>(pos.height);
    float sz = std::min(w, h);
    float r = sz / 2;

    float cx = x + w / 2;
    float cy = y + h / 2;

    // Outer circle
    renderer_->drawCircle(cx, cy, r, {255, 255, 255, 255}, {118, 118, 118, 255}, 1.0f);

    // Focus ring
    if (focused_) {
        renderer_->drawCircle(cx, cy, r + 1, {0, 0, 0, 0}, {0, 120, 215, 255}, 1.0f);
    }

    // Filled dot when checked
    const char* checked = get_attr("checked");
    if (checked) {
        renderer_->drawCircle(cx, cy, r * 0.45f, {0, 0, 0, 255}, {0, 0, 0, 0}, 0.0f);
    }
}

void ElInput::drawRange_(const litehtml::position& pos) {
    float x = static_cast<float>(pos.x);
    float y = static_cast<float>(pos.y);
    float w = static_cast<float>(pos.width);
    float h = static_cast<float>(pos.height);

    float trackH = 4.0f;
    float trackY = y + (h - trackH) / 2;
    float thumbR = 7.0f;
    float trackPad = thumbR; // Padding so thumb doesn't clip at edges

    // Track background
    renderer_->fillRoundRect(x + trackPad, trackY, w - trackPad * 2, trackH,
                            2, 2, {200, 200, 200, 255});

    // Thumb position
    float mn = rangeMin(), mx = rangeMax();
    float val = rangeValue();
    float pct = (mx > mn) ? (val - mn) / (mx - mn) : 0.0f;
    pct = std::clamp(pct, 0.0f, 1.0f);
    float thumbX = x + trackPad + pct * (w - trackPad * 2);
    float thumbY = y + h / 2;

    // Filled portion of track
    renderer_->fillRoundRect(x + trackPad, trackY, thumbX - x - trackPad, trackH,
                            2, 2, {0, 120, 215, 255});

    // Thumb
    render::Color thumbFill = dragging_ ? render::Color{0, 100, 195, 255}
                                        : render::Color{0, 120, 215, 255};
    renderer_->drawCircle(thumbX, thumbY, thumbR, thumbFill, {255, 255, 255, 255}, 1.5f);

    // Focus ring
    if (focused_) {
        renderer_->drawCircle(thumbX, thumbY, thumbR + 2, {0, 0, 0, 0}, {0, 120, 215, 128}, 1.5f);
    }
}

void ElInput::drawColor_(const litehtml::position& pos) {
    float x = static_cast<float>(pos.x);
    float y = static_cast<float>(pos.y);
    float w = static_cast<float>(pos.width);
    float h = static_cast<float>(pos.height);

    // Parse color value (default black)
    const char* val = get_attr("value");
    render::Color swatch = {0, 0, 0, 255};
    if (val && val[0] == '#' && strlen(val) == 7) {
        auto hex = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + c - 'a';
            if (c >= 'A' && c <= 'F') return 10 + c - 'A';
            return 0;
        };
        swatch.r = hex(val[1]) * 16 + hex(val[2]);
        swatch.g = hex(val[3]) * 16 + hex(val[4]);
        swatch.b = hex(val[5]) * 16 + hex(val[6]);
    }

    float pad = 3.0f;
    // Border
    renderer_->drawRect(x, y, w, h, {118, 118, 118, 255});
    // Color swatch
    renderer_->fillRect(x + pad, y + pad, w - pad * 2, h - pad * 2, swatch);

    if (focused_) {
        renderer_->drawRect(x - 1, y - 1, w + 2, h + 2, {0, 120, 215, 255});
    }
}

void ElInput::drawColorPicker() {
    if (!pickerOpen_ || !renderer_) return;

    auto font = css().get_font();
    if (!font) return;

    float px = lastDrawPos_.x;
    float py = lastDrawPos_.y + lastDrawPos_.h + 2;
    float pw = 200.0f;
    float ph = 160.0f;

    renderer_->save();
    renderer_->resetClip();

    // Background
    renderer_->fillRect(px, py, pw, ph, {255, 255, 255, 255});
    renderer_->drawRect(px, py, pw, ph, {118, 118, 118, 255});

    // Draw color grid: 10 columns x 8 rows of preset colors
    float cellW = (pw - 4) / 10.0f;
    float cellH = (ph - 4) / 8.0f;

    // HSL-based color palette
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 10; ++col) {
            float hue = col * 36.0f; // 0-360
            float sat, lit;
            if (row == 0) {
                // Grayscale row
                sat = 0.0f;
                lit = col / 9.0f;
            } else {
                sat = 1.0f;
                lit = 0.15f + (row - 1) * 0.1f; // 0.15 to 0.85
            }

            // HSL to RGB conversion
            auto hsl2rgb = [](float h, float s, float l, uint8_t& r, uint8_t& g, uint8_t& b) {
                auto hue2rgb = [](float p, float q, float t) -> float {
                    if (t < 0) t += 1;
                    if (t > 1) t -= 1;
                    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
                    if (t < 1.0f / 2) return q;
                    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
                    return p;
                };
                if (s == 0) {
                    r = g = b = static_cast<uint8_t>(l * 255);
                } else {
                    float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
                    float p = 2 * l - q;
                    float hn = h / 360.0f;
                    r = static_cast<uint8_t>(hue2rgb(p, q, hn + 1.0f / 3) * 255);
                    g = static_cast<uint8_t>(hue2rgb(p, q, hn) * 255);
                    b = static_cast<uint8_t>(hue2rgb(p, q, hn - 1.0f / 3) * 255);
                }
            };

            uint8_t cr, cg, cb;
            hsl2rgb(hue, sat, lit, cr, cg, cb);

            float cx = px + 2 + col * cellW;
            float cy = py + 2 + row * cellH;
            renderer_->fillRect(cx, cy, cellW - 1, cellH - 1, {cr, cg, cb, 255});
        }
    }

    renderer_->restore();
}

std::shared_ptr<litehtml::render_item> ElInput::create_render_item(
    const std::shared_ptr<litehtml::render_item>& parent_ri) {
    auto ret = std::make_shared<litehtml::render_item_image>(shared_from_this());
    ret->parent(parent_ri);
    return ret;
}

} // namespace bro::layout
