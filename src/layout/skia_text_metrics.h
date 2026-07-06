#pragma once

#include "layout/box.h"
#include "render/renderer.h"
#include <string>
#include <string_view>
#include <cmath>
#include <cstdlib>

namespace bro::layout {

// Implements htmlayout::layout::TextMetrics by building a render::FontRef from
// the layout-supplied family/size/weight and forwarding to the Renderer.
// The renderer caches by descriptor internally — no separate handle table.
class SkiaTextMetrics : public htmlayout::layout::TextMetrics {
public:
    explicit SkiaTextMetrics(render::Renderer* renderer)
        : renderer_(renderer) {}

    float measureWidth(std::string_view text,
                       std::string_view fontFamily,
                       float fontSize,
                       std::string_view fontWeight) override {
        auto tm = renderer_->measureText(std::string{text}, makeRef(fontFamily, fontSize, fontWeight));
        return tm.width;
    }

    float lineHeight(std::string_view fontFamily,
                     float fontSize,
                     std::string_view fontWeight) override {
        // Empty-text measureText returns just the font's vertical metrics —
        // the renderer pulls these straight from SkFontMetrics, no glyph
        // shaping needed.
        auto tm = renderer_->measureText("", makeRef(fontFamily, fontSize, fontWeight));
        // CSS line-height: normal = ascent + descent + leading, with each
        // component rounded to an integer independently — Blink rounds
        // SkFontMetrics fAscent/fDescent/fLeading per component in
        // SimpleFontData::PlatformInit, so 16px Arial is 14 + 3 + 1 = 18,
        // not round(14.48 + 3.39 + 0.52) (which happens to agree) — the
        // per-component form is exact for all sizes.
        float h = std::round(tm.ascent) + std::round(tm.descent) + std::round(tm.leading);
        return h > 0 ? h : fontSize * 1.2f;
    }

    float ascent(std::string_view fontFamily,
                 float fontSize,
                 std::string_view fontWeight) override {
        auto tm = renderer_->measureText("", makeRef(fontFamily, fontSize, fontWeight));
        // Blink rounds the font ascent to an integer (SkScalarRoundToScalar
        // on -fAscent); baselines land on integral offsets from the line top.
        float a = std::round(tm.ascent);
        // Fall back to the interface default's 80% heuristic if the backend
        // gave nothing usable.
        if (a <= 0) return 0.8f * lineHeight(fontFamily, fontSize, fontWeight);
        return a;
    }

    float xHeight(std::string_view fontFamily,
                  float fontSize,
                  std::string_view fontWeight) override {
        auto tm = renderer_->measureText("", makeRef(fontFamily, fontSize, fontWeight));
        // Real x-height from SkFontMetrics (unrounded, matching Blink).
        // Fonts that don't report one fall back to the CSS 0.5em ratio.
        return tm.xHeight > 0 ? tm.xHeight : 0.5f * fontSize;
    }

private:
    render::FontRef makeRef(std::string_view family, float size,
                            std::string_view weight) {
        int w = 400;
        if (weight == "bold" || weight == "700") w = 700;
        else if (weight == "lighter" || weight == "100") w = 100;
        else if (weight == "200") w = 200;
        else if (weight == "300") w = 300;
        else if (weight == "500") w = 500;
        else if (weight == "600") w = 600;
        else if (weight == "800") w = 800;
        else if (weight == "900") w = 900;
        else if (weight == "normal" || weight.empty()) w = 400;
        else {
            std::string ws{weight};
            char* end = nullptr;
            long v = std::strtol(ws.c_str(), &end, 10);
            if (end != ws.c_str() && v > 0) w = static_cast<int>(v);
        }
        return render::FontRef{
            family.empty() ? std::string_view{"Arial"} : family,
            size > 0 ? size : 16.0f, w, false};
    }

    render::Renderer* renderer_;
};

} // namespace bro::layout
