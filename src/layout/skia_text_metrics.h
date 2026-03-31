#pragma once

#include "layout/box.h"
#include "layout/font_manager.h"
#include "render/renderer.h"
#include <string>
#include <cmath>

namespace bro::layout {

// Implements htmlayout::layout::TextMetrics using the Renderer's font system.
class SkiaTextMetrics : public htmlayout::layout::TextMetrics {
public:
    SkiaTextMetrics(render::Renderer* renderer, FontManager* fontManager)
        : renderer_(renderer), fontManager_(fontManager) {}

    float measureWidth(const std::string& text,
                       const std::string& fontFamily,
                       float fontSize,
                       const std::string& fontWeight) override {
        uint64_t handle = getFont(fontFamily, fontSize, fontWeight);
        auto tm = renderer_->measureText(text, handle);
        return tm.width;
    }

    float lineHeight(const std::string& fontFamily,
                     float fontSize,
                     const std::string& fontWeight) override {
        uint64_t handle = getFont(fontFamily, fontSize, fontWeight);
        auto metrics = fontManager_->getMetrics(handle);
        return metrics.height > 0 ? metrics.height : fontSize * 1.2f;
    }

private:
    uint64_t getFont(const std::string& family, float size, const std::string& weight) {
        int w = 400;
        if (weight == "bold" || weight == "700") w = 700;
        else if (weight == "lighter" || weight == "100") w = 100;
        else if (weight == "200") w = 200;
        else if (weight == "300") w = 300;
        else if (weight == "500") w = 500;
        else if (weight == "600") w = 600;
        else if (weight == "800") w = 800;
        else if (weight == "900") w = 900;
        else {
            // Try parsing numeric weight
            try { w = std::stoi(weight); } catch (...) {}
        }
        return fontManager_->createFont(renderer_,
            family.empty() ? "Arial" : family, size > 0 ? size : 16.0f, w, false);
    }

    render::Renderer* renderer_;
    FontManager* fontManager_;
};

// Approximate text metrics for headless mode (no real fonts).
class HeadlessTextMetrics : public htmlayout::layout::TextMetrics {
public:
    float measureWidth(const std::string& text,
                       const std::string& /*fontFamily*/,
                       float fontSize,
                       const std::string& /*fontWeight*/) override {
        return static_cast<float>(text.size()) * fontSize * 0.6f;
    }

    float lineHeight(const std::string& /*fontFamily*/,
                     float fontSize,
                     const std::string& /*fontWeight*/) override {
        return fontSize * 1.2f;
    }
};

} // namespace bro::layout
