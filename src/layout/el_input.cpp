#include "layout/el_input.h"
#include "layout/draw_traversal.h"
#include "dom/element.h"
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bro::layout {

ElInput::ElInput(render::Renderer* renderer)
    : renderer_(renderer) {}

std::string ElInput::getAttr(const std::string& name) const {
    return elem_ ? elem_->getAttribute(name) : "";
}

ElInput::InputType ElInput::inputType(dom::Element* el) const {
    auto* e = el ? el : elem_;
    if (!e) return InputType::Text;
    std::string type = e->getAttribute("type");
    if (type.empty()) return InputType::Text;
    if (type == "password") return InputType::Password;
    if (type == "button")   return InputType::Button;
    if (type == "submit")   return InputType::Submit;
    if (type == "reset")    return InputType::Reset;
    if (type == "checkbox") return InputType::Checkbox;
    if (type == "radio")    return InputType::Radio;
    if (type == "range")    return InputType::Range;
    if (type == "number")   return InputType::Number;
    if (type == "color")    return InputType::Color;
    if (type == "hidden")   return InputType::Hidden;
    if (type == "email")    return InputType::Email;
    if (type == "tel")      return InputType::Tel;
    if (type == "url")      return InputType::Url;
    if (type == "search")   return InputType::Search;
    return InputType::Text;
}

bool ElInput::isTextType(dom::Element* el) const {
    auto t = inputType(el);
    return t == InputType::Text || t == InputType::Password ||
           t == InputType::Email || t == InputType::Tel ||
           t == InputType::Url || t == InputType::Search ||
           t == InputType::Number;
}

bool ElInput::isButtonType(dom::Element* el) const {
    auto t = inputType(el);
    return t == InputType::Button || t == InputType::Submit || t == InputType::Reset;
}

float ElInput::rangeMin() const {
    std::string a = getAttr("min");
    return a.empty() ? 0.0f : static_cast<float>(atof(a.c_str()));
}

float ElInput::rangeMax() const {
    std::string a = getAttr("max");
    return a.empty() ? 100.0f : static_cast<float>(atof(a.c_str()));
}

float ElInput::rangeStep() const {
    std::string a = getAttr("step");
    float v = a.empty() ? 0.0f : static_cast<float>(atof(a.c_str()));
    return v > 0 ? v : 1.0f;
}

float ElInput::rangeValue() const {
    std::string v = getAttr("value");
    if (!v.empty()) return static_cast<float>(atof(v.c_str()));
    return (rangeMin() + rangeMax()) / 2.0f;
}

void ElInput::setRangeValue(float v) {
    float mn = rangeMin(), mx = rangeMax(), st = rangeStep();
    v = std::clamp(v, mn, mx);
    if (st > 0) {
        v = mn + std::round((v - mn) / st) * st;
        v = std::clamp(v, mn, mx);
    }
}

void ElInput::getContentSize(float& w, float& h, float maxWidth) {
    auto t = inputType(nullptr);
    if (t == InputType::Hidden) { w = 0; h = 0; return; }
    if (t == InputType::Checkbox || t == InputType::Radio) { w = 13; h = 13; return; }
    if (t == InputType::Range) { w = 160; h = 20; return; }
    if (t == InputType::Color) { w = 44; h = 24; return; }
    if (isButtonType(nullptr)) { w = 80; h = 20; return; }
    w = (maxWidth > 0 && maxWidth < 200) ? maxWidth : 173;
    h = 20;
}

uint64_t ElInput::getFontHandle() const {
    if (!elem_ || !renderer_) return 0;
    auto& style = elem_->computedStyle();

    std::string family = "Arial";
    auto it = style.find("font-family");
    if (it != style.end() && !it->second.empty()) family = it->second;

    float size = 16.0f;
    auto sit = style.find("font-size");
    if (sit != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sit->second.c_str(), &end);
        if (end != sit->second.c_str() && v > 0) size = v;
    }

    return renderer_->createFont(family, size, 400, false);
}

void ElInput::draw(render::Renderer* renderer,
                   const htmlayout::layout::LayoutBox& box,
                   const htmlayout::css::ComputedStyle& /*style*/,
                   float offsetX, float offsetY) {
    if (!renderer_ || !elem_) return;

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    if (w <= 0 || h <= 0) return;

    lastDrawPos_ = {x, y, w, h};

    auto t = inputType(nullptr);
    if (t == InputType::Hidden) return;

    switch (t) {
        case InputType::Checkbox: drawCheckbox_(x, y, w, h); return;
        case InputType::Radio:    drawRadio_(x, y, w, h); return;
        case InputType::Range:    drawRange_(x, y, w, h); return;
        case InputType::Color:    drawColor_(x, y, w, h); return;
        default: break;
    }
    drawText_(x, y, w, h);
}

void ElInput::drawText_(float x, float y, float w, float h) {
    std::string val = getAttr("value");
    std::string placeholder = getAttr("placeholder");
    std::string text;
    bool isPlaceholder = false;

    if (!val.empty()) {
        if (inputType(nullptr) == InputType::Password) {
            text = std::string(val.size(), '*');
        } else {
            text = val;
        }
    } else if (!focused_ && !placeholder.empty()) {
        text = placeholder;
        isPlaceholder = true;
    }

    uint64_t fontHandle = getFontHandle();
    if (!fontHandle) return;

    auto tm = renderer_->measureText("M", fontHandle);
    float fontHeight = tm.height;
    float ascent = tm.ascent > 0 ? tm.ascent : fontHeight * 0.8f;
    float textY = y + (h - fontHeight) / 2.0f + ascent;
    float padX = 4.0f;
    float drawX = x + padX;

    renderer_->save();
    renderer_->setClip(x, y, w, h);

    if (!text.empty()) {
        // Use the element's computed color for text (respects app themes)
        render::Color textColor = {0, 0, 0, 255};
        if (elem_) {
            auto& style = elem_->computedStyle();
            auto cIt = style.find("color");
            if (cIt != style.end() && !cIt->second.empty()) {
                render::Color parsed;
                if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    textColor = parsed;
                }
            }
        }
        render::Color color = isPlaceholder ? render::Color{128, 128, 128, 180}
                                            : textColor;
        renderer_->drawText(text, drawX, textY, fontHandle, color);
    }

    if (focused_ && isTextType(nullptr)) {
        // Use computed color for cursor too
        render::Color cursorColor = {0, 0, 0, 255};
        if (elem_) {
            auto& style = elem_->computedStyle();
            auto cIt = style.find("color");
            if (cIt != style.end() && !cIt->second.empty()) {
                render::Color parsed;
                if (DrawTraversal::tryParseColor(cIt->second, parsed)) {
                    cursorColor = parsed;
                }
            }
        }
        int cpos = std::clamp(cursorPos_, 0, static_cast<int>(val.size()));
        std::string beforeCursor = val.substr(0, cpos);
        float cursorX = drawX;
        if (!beforeCursor.empty()) {
            auto ctm = renderer_->measureText(beforeCursor, fontHandle);
            cursorX += ctm.width;
        }
        float cursorTop = y + (h - fontHeight) / 2.0f;
        float cursorBottom = cursorTop + fontHeight;
        renderer_->drawLine(cursorX, cursorTop, cursorX, cursorBottom, cursorColor, 1.0f);
    }

    if (inputType(nullptr) == InputType::Number) {
        float btnW = 16.0f;
        float bx = x + w - btnW;
        renderer_->drawLine(bx, y, bx, y + h, {180, 180, 180, 255}, 1.0f);
        renderer_->drawLine(bx, y + h / 2, bx + btnW, y + h / 2, {180, 180, 180, 255}, 1.0f);

        float cx = bx + btnW / 2;
        render::PointF upPts[3] = {
            {cx - 4, y + h / 4 + 2}, {cx + 4, y + h / 4 + 2}, {cx, y + h / 4 - 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(upPts, 3),
                              {80, 80, 80, 255}, {0, 0, 0, 0}, 0.0f);

        render::PointF downPts[3] = {
            {cx - 4, y + h * 3 / 4 - 2}, {cx + 4, y + h * 3 / 4 - 2}, {cx, y + h * 3 / 4 + 2}
        };
        renderer_->drawPolygon(std::span<const render::PointF>(downPts, 3),
                              {80, 80, 80, 255}, {0, 0, 0, 0}, 0.0f);
    }

    renderer_->restore();
}

void ElInput::drawCheckbox_(float x, float y, float w, float h) {
    float sz = std::min(w, h);
    float bx = x + (w - sz) / 2;
    float by = y + (h - sz) / 2;

    renderer_->fillRect(bx, by, sz, sz, {255, 255, 255, 255});
    renderer_->drawRect(bx, by, sz, sz, {118, 118, 118, 255});

    if (focused_) {
        renderer_->drawRect(bx - 1, by - 1, sz + 2, sz + 2, {0, 120, 215, 255});
    }

    if (elem_ && elem_->hasAttribute("checked")) {
        float pad = sz * 0.2f;
        float x1 = bx + pad, y1 = by + sz * 0.5f;
        float x2 = bx + sz * 0.4f, y2 = by + sz - pad;
        float x3 = bx + sz - pad, y3 = by + pad;
        renderer_->drawLine(x1, y1, x2, y2, {0, 0, 0, 255}, 2.0f);
        renderer_->drawLine(x2, y2, x3, y3, {0, 0, 0, 255}, 2.0f);
    }
}

void ElInput::drawRadio_(float x, float y, float w, float h) {
    float sz = std::min(w, h);
    float r = sz / 2;
    float cx = x + w / 2, cy = y + h / 2;

    renderer_->drawCircle(cx, cy, r, {255, 255, 255, 255}, {118, 118, 118, 255}, 1.0f);
    if (focused_) {
        renderer_->drawCircle(cx, cy, r + 1, {0, 0, 0, 0}, {0, 120, 215, 255}, 1.0f);
    }
    if (elem_ && elem_->hasAttribute("checked")) {
        renderer_->drawCircle(cx, cy, r * 0.45f, {0, 0, 0, 255}, {0, 0, 0, 0}, 0.0f);
    }
}

void ElInput::drawRange_(float x, float y, float w, float h) {
    float trackH = 4.0f;
    float trackY = y + (h - trackH) / 2;
    float thumbR = 7.0f;
    float trackPad = thumbR;

    renderer_->fillRoundRect(x + trackPad, trackY, w - trackPad * 2, trackH,
                            2, 2, {200, 200, 200, 255});

    float mn = rangeMin(), mx = rangeMax();
    float val = rangeValue();
    float pct = (mx > mn) ? (val - mn) / (mx - mn) : 0.0f;
    pct = std::clamp(pct, 0.0f, 1.0f);
    float thumbX = x + trackPad + pct * (w - trackPad * 2);
    float thumbY = y + h / 2;

    renderer_->fillRoundRect(x + trackPad, trackY, thumbX - x - trackPad, trackH,
                            2, 2, {0, 120, 215, 255});

    render::Color thumbFill = dragging_ ? render::Color{0, 100, 195, 255}
                                        : render::Color{0, 120, 215, 255};
    renderer_->drawCircle(thumbX, thumbY, thumbR, thumbFill, {255, 255, 255, 255}, 1.5f);

    if (focused_) {
        renderer_->drawCircle(thumbX, thumbY, thumbR + 2, {0, 0, 0, 0}, {0, 120, 215, 128}, 1.5f);
    }
}

void ElInput::drawColor_(float x, float y, float w, float h) {
    std::string val = getAttr("value");
    render::Color swatch = {0, 0, 0, 255};
    if (val.size() == 7 && val[0] == '#') {
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
    renderer_->drawRect(x, y, w, h, {118, 118, 118, 255});
    renderer_->fillRect(x + pad, y + pad, w - pad * 2, h - pad * 2, swatch);
    if (focused_) {
        renderer_->drawRect(x - 1, y - 1, w + 2, h + 2, {0, 120, 215, 255});
    }
}

void ElInput::drawColorPicker() {
    if (!pickerOpen_ || !renderer_) return;

    float px = lastDrawPos_.x;
    float py = lastDrawPos_.y + lastDrawPos_.h + 2;
    float pw = 200.0f;
    float ph = 160.0f;

    renderer_->save();
    renderer_->resetClip();

    renderer_->fillRect(px, py, pw, ph, {255, 255, 255, 255});
    renderer_->drawRect(px, py, pw, ph, {118, 118, 118, 255});

    float cellW = (pw - 4) / 10.0f;
    float cellH = (ph - 4) / 8.0f;

    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 10; ++col) {
            float hue = col * 36.0f;
            float sat, lit;
            if (row == 0) {
                sat = 0.0f;
                lit = col / 9.0f;
            } else {
                sat = 1.0f;
                lit = 0.15f + (row - 1) * 0.1f;
            }

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

} // namespace bro::layout
