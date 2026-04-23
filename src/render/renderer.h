#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <string>
#include <span>
#include <vector>

class SkCanvas;
class SkImageFilter;
class SkSurface;

namespace bro::render {

struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

struct TextMetrics {
    float width = 0.0f;
    float height = 0.0f;
    float ascent = 0.0f;   // distance from baseline to top (positive value)
    float descent = 0.0f;  // distance from baseline to bottom (positive value)
};

// Derived font line metrics for text layout and vertical centering.
// Use lineHeight() (ascent + descent) for centering, not TextMetrics::height
// which is the tight bounding box of a specific glyph.
struct LineMetrics {
    float ascent;
    float descent;

    float lineHeight() const { return ascent + descent; }

    // Compute baseline Y for text vertically centered in a box of height h at position y
    float baselineY(float y, float h) const {
        return y + (h - lineHeight()) / 2.0f + ascent;
    }

    static LineMetrics from(const TextMetrics& tm) {
        return {
            tm.ascent > 0 ? tm.ascent : tm.height * 0.8f,
            tm.descent > 0 ? tm.descent : tm.height * 0.2f
        };
    }
};

struct PointF {
    float x = 0;
    float y = 0;
};

struct ColorStop {
    float offset;   // 0.0–1.0
    Color color;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void clear(Color color) = 0;

    virtual void drawRect(float x, float y, float w, float h, Color color) = 0;
    virtual void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) = 0;
    virtual void fillRect(float x, float y, float w, float h, Color color) = 0;
    virtual void fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) = 0;

    virtual void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) = 0;
    virtual TextMetrics measureText(std::string_view text, uint64_t font_handle) = 0;

    virtual uint64_t createFont(std::string_view family, float size, int weight, bool italic) = 0;
    virtual void deleteFont(uint64_t font_handle) = 0;

    // Register a custom font from file data (for @font-face support).
    // Returns true on success. The font will be used when createFont matches
    // the given family name, weight, and style.
    virtual bool registerCustomFont(const std::string& family,
                                    const void* data, size_t len,
                                    int weight, bool italic) { return false; }

    virtual void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) = 0;
    virtual void drawImage(const void* data, size_t len, float x, float y, float w, float h) = 0;

    // Draw a raw RGBA8 buffer. Unlike drawImage(), no codec decode happens —
    // `rgba` must be tightly-packed or use `stride` for row padding. Used by
    // video playback where decoding PNG/JPEG per frame would be absurd.
    // Default implementation is a no-op so backends can adopt incrementally.
    virtual void drawPixelsRGBA(const uint8_t* /*rgba*/,
                                int /*srcW*/, int /*srcH*/, int /*stride*/,
                                float /*x*/, float /*y*/, float /*w*/, float /*h*/) {}

    // SVG drawing primitives
    virtual void drawCircle(float cx, float cy, float r,
                            Color fill, Color stroke, float strokeWidth) = 0;
    virtual void drawEllipse(float cx, float cy, float rx, float ry,
                             Color fill, Color stroke, float strokeWidth) = 0;
    virtual void drawPath(std::string_view svgPathData,
                          Color fill, Color stroke, float strokeWidth) = 0;
    virtual void drawPolygon(std::span<const PointF> points,
                             Color fill, Color stroke, float strokeWidth) = 0;
    virtual void drawPolyline(std::span<const PointF> points,
                              Color stroke, float strokeWidth) = 0;

    // Box shadow: draw a shadow behind a rect (or rounded rect if rx > 0)
    virtual void drawBoxShadow(float x, float y, float w, float h,
                               float rx, float ry,
                               float offsetX, float offsetY,
                               float blur, float spread,
                               Color color, bool inset) = 0;

    // Canvas state (for SVG coordinate transforms)
    virtual void save() = 0;
    virtual void restore() = 0;

    // Save a layer with the given opacity (0-255). Everything drawn until
    // the matching restore() is composited at this opacity.
    virtual void saveLayerAlpha(uint8_t alpha) = 0;
    virtual void translate(float dx, float dy) = 0;
    virtual void scale(float sx, float sy) = 0;
    virtual void rotate(float degrees) = 0;

    // Concatenate a 2D affine matrix [a b c d e f] (column-major: a,b = first col,
    // c,d = second col, e,f = translation).  Maps to CSS matrix(a,b,c,d,e,f).
    virtual void concat(float a, float b, float c, float d, float e, float f) = 0;

    // Save a layer with a CSS image filter applied. Everything drawn until
    // the matching restore() passes through the filter.  Takes ownership of
    // the filter reference.  If filter is null, behaves like save().
    virtual void saveLayerWithFilter(SkImageFilter* filter,
                                     float x, float y, float w, float h) = 0;

    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void resetClip() = 0;

    // Gradient fills (color stops must be sorted by offset 0..1)
    virtual void fillLinearGradient(float x, float y, float w, float h,
                                    float startX, float startY, float endX, float endY,
                                    std::span<const ColorStop> stops) = 0;
    virtual void fillRadialGradient(float x, float y, float w, float h,
                                    float cx, float cy, float rx, float ry,
                                    std::span<const ColorStop> stops) = 0;
    virtual void fillConicGradient(float x, float y, float w, float h,
                                   float cx, float cy, float angleDeg,
                                   std::span<const ColorStop> stops) = 0;

    virtual void beginFrame(int width, int height) = 0;
    virtual void endFrame() = 0;

    // Screenshot support — access the underlying Skia surface/canvas.
    virtual SkCanvas* getCanvas() const { return nullptr; }
    virtual SkSurface* surface() const { return nullptr; }
    virtual bool saveScreenshot(const std::string& path) { return false; }

    /// Capture the surface as RGBA pixels (w x h x 4). Returns empty on failure.
    virtual std::vector<uint8_t> capturePixels() { return {}; }

};

} // namespace bro::render
