#pragma once

#include "render/renderer.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
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

    void clear(Color c) override;
    void drawRect(float x, float y, float w, float h, Color c) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color c) override;
    void fillRect(float x, float y, float w, float h, Color c) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color c) override;

    void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color c) override;
    TextMetrics measureText(std::string_view text, uint64_t font_handle) override;

    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t h) override;

    void drawLine(float x1, float y1, float x2, float y2, Color c, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override;

    void drawCircle(float cx, float cy, float r,
                    Color fill, Color stroke, float strokeWidth) override;
    void drawEllipse(float cx, float cy, float rx, float ry,
                     Color fill, Color stroke, float strokeWidth) override;
    void drawPath(std::string_view svgPathData,
                  Color fill, Color stroke, float strokeWidth) override;
    void drawPolygon(std::span<const PointF> points,
                     Color fill, Color stroke, float strokeWidth) override;
    void drawPolyline(std::span<const PointF> points,
                      Color stroke, float strokeWidth) override;

    void save() override;
    void restore() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;

    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;

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

    /// Access the underlying Skia canvas (valid between beginFrame/endFrame).
    SkCanvas* getCanvas() const { return canvas_; }

    /// Access the raster surface (valid after beginFrame).
    SkSurface* surface() const { return surface_.get(); }

    /// Save the current surface to a PNG file.
    bool saveScreenshot(const std::string& path);

private:
    static SkColor toSkColor(Color c);

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };

    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t nextHandle_ = 1;
};

} // namespace bro::render
