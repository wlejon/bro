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
        // CSS line-height: normal = ascent + descent + leading (Chromium parity).
        float h = std::round(tm.ascent + tm.descent + tm.leading);
        return h > 0 ? h : fontSize * 1.2f;
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
