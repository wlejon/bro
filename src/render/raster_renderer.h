#pragma once

#include "render/renderer.h"
#include "render/font_fallback.h"
#include "render/image_cache.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkFontStyle.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace bro::render {

/// CPU-only Skia renderer for headless mode.
/// Renders to an in-memory raster surface with real font metrics.
class RasterRenderer final : public Renderer {
public:
    RasterRenderer() = default;
    ~RasterRenderer() override = default;

    void clear(bromath::Color c) override;
    void drawRect(float x, float y, float w, float h, bromath::Color c) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color c) override;
    void fillRect(float x, float y, float w, float h, bromath::Color c) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color c) override;
    void fillRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, bromath::Color color) override;
    void drawRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, float strokeWidth, bromath::Color color) override;
    void setClipRRect(float x, float y, float w, float h, const Radii& r) override;
    void drawBoxShadowRadii(float x, float y, float w, float h, const Radii& r,
                            float offsetX, float offsetY,
                            float blur, float spread,
                            bromath::Color color, bool inset) override;

    void drawText(std::string_view text, float x, float y, FontRef font, bromath::Color c) override;
    void drawTextEx(std::string_view text, float x, float y,
                    FontRef font, bromath::Color c,
                    float letterSpacing, float blur) override;
    TextMetrics measureText(std::string_view text, FontRef font) override;

    void drawLine(float x1, float y1, float x2, float y2, bromath::Color c, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h,
                   uint64_t imageId) override;
    void drawPixelsRGBA(const uint8_t* rgba, int srcW, int srcH, int stride,
                        float x, float y, float w, float h) override;
    void drawSvgMarkup(const char* data, size_t len,
                       float x, float y, float w, float h) override;

    void drawCircle(float cx, float cy, float r,
                    bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawEllipse(float cx, float cy, float rx, float ry,
                     bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPath(std::string_view svgPathData,
                  bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPolygon(std::span<const PointF> points,
                     bromath::Color fill, bromath::Color stroke, float strokeWidth) override;
    void drawPolyline(std::span<const PointF> points,
                      bromath::Color stroke, float strokeWidth) override;

    void drawBoxShadow(float x, float y, float w, float h,
                       float rx, float ry,
                       float offsetX, float offsetY,
                       float blur, float spread,
                       bromath::Color color, bool inset) override;

    void save() override;
    void restore() override;
    void saveLayerAlpha(uint8_t alpha) override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void rotate(float degrees) override;
    void concat(float a, float b, float c, float d, float e, float f) override;
    void concat4x4(const float m[16]) override;
    void saveLayerWithFilter(std::span<const CssFilterParams> filters,
                             float x, float y, float w, float h) override;
    void saveLayerWithBlend(BlendMode mode) override;

    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;
    void setClipPolygon(std::span<const render::PointF> points) override;

    void fillLinearGradient(float x, float y, float w, float h,
                            float startX, float startY, float endX, float endY,
                            std::span<const ColorStop> stops) override;
    void fillRadialGradient(float x, float y, float w, float h,
                            float cx, float cy, float rx, float ry,
                            std::span<const ColorStop> stops) override;
    void fillConicGradient(float x, float y, float w, float h,
                           float cx, float cy, float angleDeg,
                           std::span<const ColorStop> stops) override;

    void beginFrame(int width, int height) override;
    void endFrame() override;

    SkCanvas* getCanvas() const override { return canvas_; }
    SkSurface* surface() const override { return surface_.get(); }
    bool saveScreenshot(const std::string& path) override;
    std::vector<uint8_t> capturePixels() override;

private:
    static SkColor toSkColor(bromath::Color c);

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
        SkFontStyle style;
    };
    struct FontKey {
        std::string family;
        float size;
        int weight;
        bool italic;
        bool operator==(const FontKey&) const = default;
    };
    struct FontKeyHash {
        size_t operator()(const FontKey& k) const noexcept {
            size_t h = std::hash<std::string>{}(k.family);
            auto mix = [&](size_t v) {
                h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            };
            mix(std::hash<float>{}(k.size));
            mix(std::hash<int>{}(k.weight));
            mix(std::hash<bool>{}(k.italic));
            return h;
        }
    };

    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    std::unordered_map<FontKey, FontEntry, FontKeyHash> fonts_;

    // Decoded-image cache — see DecodedImageCache. Swept once per beginFrame().
    DecodedImageCache imageCache_;

    const FontEntry* getOrCreateFont(FontRef font);

    sk_sp<SkFontMgr> fontMgr_;
    FontFallbackCache fallbackCache_;
    SkFontMgr* ensureFontMgr();
};

} // namespace bro::render
