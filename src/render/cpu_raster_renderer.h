#pragma once

#include <memory>
#include <unordered_map>

#include <glad/gl.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFont.h>
#include <include/core/SkTypeface.h>

#include "render/renderer.h"

namespace bro::render { class GLContext; }

namespace bro::render {

/// CPU-raster Renderer using a Skia raster surface.
/// Uploads pixels to a GL texture for compositing in windowed mode.
class CPURasterRenderer : public Renderer {
public:
    explicit CPURasterRenderer(GLContext* gl);
    ~CPURasterRenderer() override;

    void clear(Color color) override;
    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;
    void fillRect(float x, float y, float w, float h, Color color) override;
    void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) override;
    TextMetrics measureText(std::string_view text, uint64_t font_handle) override;
    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t font_handle) override;
    void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;
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
    void drawBoxShadow(float x, float y, float w, float h,
                       float rx, float ry,
                       float offsetX, float offsetY,
                       float blur, float spread,
                       Color color, bool inset) override;
    void save() override;
    void restore() override;
    void saveLayerAlpha(uint8_t alpha) override;
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

    /// Upload rasterized pixels to GL texture (no-op if no GLContext).
    void uploadToGPU();

    /// Get the GL texture containing the rendered output (premultiplied alpha).
    GLuint getTexture() const { return texture_; }

    /// Get the Skia surface (for headless screenshot compositing).
    SkSurface* surface() const { return surface_.get(); }

private:
    SkColor toSkColor(Color c) const;

    GLContext* gl_;
    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    GLuint texture_ = 0;
    int texWidth_ = 0;
    int texHeight_ = 0;

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t nextFontHandle_ = 1;
};

} // namespace bro::render
