#include "layout/el_input.h"
#include "layout/draw_traversal.h"
#include "dom/element.h"
#include "render/renderer.h"
#include "util/platform.h"

#include <SDL3/SDL_keycode.h>
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

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

KeyHandleResult ElInput::handleKeyDown(dom::Element* el, int keycode, int mod) {
    KeyHandleResult r;
    auto itype = inputType(el);

    // Checkbox/radio: space toggles
    if ((itype == InputType::Checkbox || itype == InputType::Radio)
        && keycode == SDLK_SPACE) {
        if (itype == InputType::Checkbox) {
            if (el->hasAttribute("checked"))
                el->removeAttribute("checked");
            else
                el->setAttribute("checked", "");
        } else {
            el->setAttribute("checked", "");
        }
        r.handled = true;
        r.dispatchChange = true;
        r.dispatchInput = true;
        return r;
    }

    // Range: arrow keys adjust value
    if (itype == InputType::Range) {
        if (keycode == SDLK_LEFT || keycode == SDLK_DOWN) {
            float v = std::clamp(rangeValue() - rangeStep(), rangeMin(), rangeMax());
            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            el->setAttribute("value", buf);
            r.handled = true;
            r.dispatchInput = true;
            return r;
        }
        if (keycode == SDLK_RIGHT || keycode == SDLK_UP) {
            float v = std::clamp(rangeValue() + rangeStep(), rangeMin(), rangeMax());
            char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
            el->setAttribute("value", buf);
            r.handled = true;
            r.dispatchInput = true;
            return r;
        }
    }

    // Non-text types: nothing more to handle
    if (!isTextType(el)) return r;

    // Text editing
    std::string val = el->getAttribute("value");
    int pos = std::clamp(cursorPos_, 0, static_cast<int>(val.size()));

    if (keycode == SDLK_BACKSPACE) {
        if (pos > 0) {
            r.inputData = val.substr(pos - 1, 1);
            val.erase(pos - 1, 1);
            setCursorPos(pos - 1);
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentBackward";
        }
        r.handled = true;
    } else if (keycode == SDLK_DELETE) {
        if (pos < static_cast<int>(val.size())) {
            r.inputData = val.substr(pos, 1);
            val.erase(pos, 1);
            el->setAttribute("value", val);
            r.dispatchInput = true;
            r.inputType = "deleteContentForward";
        }
        r.handled = true;
    } else if (keycode == SDLK_LEFT) {
        if (pos > 0) setCursorPos(pos - 1);
        r.handled = true;
    } else if (keycode == SDLK_RIGHT) {
        if (pos < static_cast<int>(val.size())) setCursorPos(pos + 1);
        r.handled = true;
    } else if (keycode == SDLK_HOME) {
        setCursorPos(0);
        r.handled = true;
    } else if (keycode == SDLK_END) {
        setCursorPos(static_cast<int>(val.size()));
        r.handled = true;
    } else if (itype == InputType::Number &&
               (keycode == SDLK_UP || keycode == SDLK_DOWN)) {
        float v = val.empty() ? 0.0f : static_cast<float>(atof(val.c_str()));
        float step = rangeStep();
        v += (keycode == SDLK_UP) ? step : -step;
        std::string minStr = el->getAttribute("min");
        std::string maxStr = el->getAttribute("max");
        if (!minStr.empty()) v = std::max(v, rangeMin());
        if (!maxStr.empty()) v = std::min(v, rangeMax());
        char buf[64]; snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        el->setAttribute("value", buf);
        setCursorPos(static_cast<int>(strlen(buf)));
        r.handled = true;
        r.dispatchInput = true;
    } else if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER ||
               keycode == SDLK_ESCAPE) {
        setFocused(false);
        r.handled = true;
        r.unfocus = true;
    } else if (util::hasPrimaryMod(mod) && keycode == SDLK_A) {
        setCursorPos(static_cast<int>(val.size()));
        r.handled = true;
    }

    return r;
}

KeyHandleResult ElInput::handleTextInput(dom::Element* el, const std::string& text) {
    KeyHandleResult r;
    if (!focused_ || !isTextType(el)) return r;

    // Number type: only allow numeric characters
    if (inputType(el) == InputType::Number) {
        for (char c : text) {
            if (!((c >= '0' && c <= '9') || c == '-' || c == '.' ||
                  c == 'e' || c == 'E' || c == '+'))
                return r;
        }
    }

    std::string val = el->getAttribute("value");
    int pos = std::clamp(cursorPos_, 0, static_cast<int>(val.size()));
    val.insert(pos, text);
    setCursorPos(pos + static_cast<int>(text.size()));
    el->setAttribute("value", val);

    r.handled = true;
    r.dispatchInput = true;
    r.inputData = text;
    r.inputType = "insertText";
    return r;
}

void ElInput::getContentSize(float& w, float& h, float maxWidth) {
    auto t = inputType(nullptr);
    if (t == InputType::Hidden) { w = 0; h = 0; return; }

    // Read dimensions from computed style (set by UA stylesheet)
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto wIt = style.find("width");
        auto hIt = style.find("height");
        if (wIt != style.end() && !wIt->second.empty() && wIt->second != "auto") {
            char* end = nullptr;
            float v = std::strtof(wIt->second.c_str(), &end);
            if (end != wIt->second.c_str() && v > 0) w = v;
        }
        if (hIt != style.end() && !hIt->second.empty() && hIt->second != "auto") {
            char* end = nullptr;
            float v = std::strtof(hIt->second.c_str(), &end);
            if (end != hIt->second.c_str() && v > 0) h = v;
        }
        if (w > 0 && h > 0) return;
    }

    // Fallback defaults if style didn't provide dimensions
    if (t == InputType::Checkbox || t == InputType::Radio) { w = 13; h = 13; return; }
    if (t == InputType::Range) { w = 160; h = 20; return; }
    if (t == InputType::Color) { w = 44; h = 24; return; }
    if (isButtonType(nullptr)) {
        // Button-type inputs render the `value` attribute as their label.
        // Measure the label so the box is large enough — otherwise padding
        // + borders swamp the 80px default and the text gets clipped.
        float labelW = 0.f;
        if (elem_ && renderer_) {
            std::string val = elem_->getAttribute("value");
            if (!val.empty()) {
                auto tm = renderer_->measureText(val, getFontRef());
                labelW = tm.width;
            }
        }
        w = std::max(80.0f, std::ceil(labelW));
        h = 20;
        return;
    }
    w = (maxWidth > 0 && maxWidth < 200) ? maxWidth : 173;
    h = 20;
}

render::FontRef ElInput::getFontRef() const {
    if (!elem_) return {std::string_view{"Arial"}, 16.0f, 400, false};

    auto& style = elem_->computedStyle();

    std::string_view family = "Arial";
    auto it = style.find("font-family");
    if (it != style.end() && !it->second.empty()) family = it->second;

    float size = 16.0f;
    auto sit = style.find("font-size");
    if (sit != style.end()) {
        char* end = nullptr;
        float v = std::strtof(sit->second.c_str(), &end);
        if (end != sit->second.c_str() && v > 0) size = v;
    }

    return render::FontRef{family, size, 400, false};
}

void ElInput::draw(render::Renderer* renderer,
                   const htmlayout::layout::LayoutBox& box,
                   const htmlayout::css::ComputedStyle& /*style*/,
                   float offsetX, float offsetY) {
    if (!renderer || !elem_) return;

    // Use the caller's renderer (may differ from construction renderer,
    // e.g. raster thread has its own SkiaRenderer). Leave it set — the
    // raster thread is idle when the main thread uses control methods,
    // so no race condition.
    renderer_ = renderer;

    float x = box.contentRect.x + offsetX;
    float y = box.contentRect.y + offsetY;
    float w = box.contentRect.width;
    float h = box.contentRect.height;

    if (w <= 0 || h <= 0) return;

    lastDrawPos_ = {x, y, w, h};

    auto t = inputType(nullptr);
    if (t == InputType::Hidden) return;

    switch (t) {
        case InputType::Checkbox: drawCheckbox_(x, y, w, h); break;
        case InputType::Radio:    drawRadio_(x, y, w, h); break;
        case InputType::Range:    drawRange_(x, y, w, h); break;
        case InputType::Color:    drawColor_(x, y, w, h); break;
        default: drawText_(x, y, w, h); break;
    }
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

    render::FontRef fontRef = getFontRef();
    auto lm = render::LineMetrics::from(renderer_->measureText("M", fontRef));
    float textY = lm.baselineY(y, h);
    float drawX = x;

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
        renderer_->drawText(text, drawX, textY, fontRef, color);
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
            auto ctm = renderer_->measureText(beforeCursor, fontRef);
            cursorX += ctm.width;
        }
        float cursorTop = textY - lm.ascent;
        float cursorBottom = cursorTop + lm.lineHeight();
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

float ElInput::rangeThumbRadius(float h) {
    // Scale with element height, but keep the thumb inside the hit box
    // (diameter <= h). 0.4 * h gives ~8 at the default 20px height.
    float r = std::clamp(h * 0.4f, 2.0f, 10.0f);
    return std::min(r, h * 0.5f);
}

float ElInput::rangeTrackHeight(float h) {
    return std::clamp(h * 0.2f, 2.0f, 6.0f);
}

void ElInput::drawRange_(float x, float y, float w, float h) {
    float trackH = rangeTrackHeight(h);
    float trackY = y + (h - trackH) / 2;
    float thumbR = rangeThumbRadius(h);
    float trackPad = thumbR;

    // Accent color — honor CSS accent-color for the filled track and thumb,
    // falling back to Windows-blue when unset.
    render::Color accent = {0, 120, 215, 255};
    if (elem_) {
        auto& style = elem_->computedStyle();
        auto it = style.find("accent-color");
        if (it != style.end() && !it->second.empty() && it->second != "auto") {
            accent = DrawTraversal::parseColor(it->second);
        }
    }
    render::Color accentDark = {
        static_cast<uint8_t>(accent.r * 0.82f),
        static_cast<uint8_t>(accent.g * 0.82f),
        static_cast<uint8_t>(accent.b * 0.82f),
        accent.a
    };
    render::Color focusRing = {accent.r, accent.g, accent.b, 128};

    renderer_->fillRoundRect(x + trackPad, trackY, w - trackPad * 2, trackH,
                            2, 2, {200, 200, 200, 255});

    float mn = rangeMin(), mx = rangeMax();
    float val = rangeValue();
    float pct = (mx > mn) ? (val - mn) / (mx - mn) : 0.0f;
    pct = std::clamp(pct, 0.0f, 1.0f);
    float thumbX = x + trackPad + pct * (w - trackPad * 2);
    float thumbY = y + h / 2;

    renderer_->fillRoundRect(x + trackPad, trackY, thumbX - x - trackPad, trackH,
                            2, 2, accent);

    render::Color thumbFill = dragging_ ? accentDark : accent;
    renderer_->drawCircle(thumbX, thumbY, thumbR, thumbFill, {255, 255, 255, 255}, 1.5f);

    if (focused_) {
        renderer_->drawCircle(thumbX, thumbY, thumbR + 2, {0, 0, 0, 0}, focusRing, 1.5f);
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

} // namespace bro::layout
