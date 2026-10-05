#include "layout/el_input.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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
        // Button-type inputs (submit/reset/button) shrink to fit their label
        // just like <button> — content width = label text width, content
        // height = one line. The UA button box model (padding 1px 6px,
        // 2px border, border-box) then wraps it, matching Chromium exactly.
        float labelW = 0.f;
        float lineH = 16.0f;
        if (elem_ && renderer_) {
            auto fr = getFontRef();
            std::string val = elem_->getAttribute("value");
            if (!val.empty()) labelW = renderer_->measureText(val, fr).width;
            auto lm = render::LineMetrics::from(renderer_->measureText("M", fr));
            if (lm.lineHeight() > 0) lineH = lm.lineHeight();
        }
        w = labelW;
        h = lineH;
        return;
    }

    // Text-like inputs: width derives from the `size` attribute (default 20)
    // times the font's average lowercase advance, plus a fixed decoration
    // allowance — matching Chromium's default text-field metrics (size=20 at
    // 16px sans → 195px content; at 13.333px → 169px). Height is one line.
    int sizeAttr = 20;
    {
        std::string s = getAttr("size");
        if (!s.empty()) { int v = atoi(s.c_str()); if (v > 0) sizeAttr = v; }
    }
    float avgChar = 8.0f;
    float lineH = 16.0f;
    if (renderer_) {
        auto fr = getFontRef();
        float alpha = renderer_->measureText("abcdefghijklmnopqrstuvwxyz", fr).width;
        if (alpha > 0) avgChar = alpha / 26.0f;
        auto lm = render::LineMetrics::from(renderer_->measureText("M", fr));
        float lh = lm.lineHeight();
        if (lh > 0) lineH = lh;
    }
    w = std::round(sizeAttr * avgChar + 38.4f);
    // Don't overflow a narrow containing box (preserves the prior clamp for
    // inputs placed in tight columns).
    if (maxWidth > 0 && maxWidth < 200 && maxWidth < w) w = maxWidth;
    h = lineH;
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

std::string ElInput::displayText_() const {
    std::string val = getAttr("value");
    if (inputType(nullptr) == InputType::Password) {
        return std::string(val.size(), '*');
    }
    return val;
}

ElInput::DrawPos ElInput::contentBox_() const {
    if (!elem_) return {0, 0, 0, 0};
    auto r = dom::absoluteContentBox(elem_);
    return {r.x + docOffsetX_, r.y + docOffsetY_, r.width, r.height};
}

} // namespace bro::layout
