#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <string>
#include <span>
#include <vector>

class SkCanvas;
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
    float leading = 0.0f;  // recommended extra line gap (SkFontMetrics::fLeading)
};

// Derived font line metrics for text layout and vertical centering.
// Use lineHeight() (ascent + descent) for centering, not TextMetrics::height
// which is the tight bounding box of a specific glyph.
struct LineMetrics {
    float ascent;
    float descent;
    float leading = 0.0f;

    float lineHeight() const { return ascent + descent; }

    // Compute baseline Y for text vertically centered in a box of height h at position y
    float baselineY(float y, float h) const {
        return y + (h - lineHeight()) / 2.0f + ascent;
    }

    static LineMetrics from(const TextMetrics& tm) {
        return {
            tm.ascent > 0 ? tm.ascent : tm.height * 0.8f,
            tm.descent > 0 ? tm.descent : tm.height * 0.2f,
            tm.leading
        };
    }
};

struct PointF {
    float x = 0;
    float y = 0;
};

// Per-corner border radii (CSS border-radius). Each corner has independent
// horizontal (x) and vertical (y) radii. Order matches SkRRect corner enum:
// [0]=top-left, [1]=top-right, [2]=bottom-right, [3]=bottom-left.
struct Radii {
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};
    bool isZero() const {
        return x[0] == 0 && x[1] == 0 && x[2] == 0 && x[3] == 0 &&
               y[0] == 0 && y[1] == 0 && y[2] == 0 && y[3] == 0;
    }
};

struct ColorStop {
    float offset;   // 0.0–1.0
    Color color;
};

// Value-typed font reference — replaces the opaque `uint64_t font_handle` that
// drawText/measureText used to take. Each renderer caches by content
// internally, so the same FontRef across calls hits the same cached SkFont
// without needing a per-renderer handle to be threaded through layout. The
// `family` view must outlive the call but does not need to outlive the
// returned cache entry — renderer implementations copy the family into their
// own keys on cache miss.
struct FontRef {
    std::string_view family;
    float            size = 14.0f;
    int              weight = 400;   // CSS numeric (400 = normal, 700 = bold)
    bool             italic = false;
};

// CSS filter primitive — a single function in a `filter:` chain. The list is
// applied in order to produce the final image filter. Backends are responsible
// for translating these descriptors into their native filter representation
// (e.g. SkImageFilter on Skia backends). DrawTraversal records descriptors
// only; it does not construct backend filter objects.
struct CssFilterParams {
    enum Kind {
        Blur,         // a = sigma (px)
        Brightness,   // a = factor
        Contrast,     // a = factor
        Grayscale,    // a = amount [0..1]
        Sepia,        // a = amount [0..1]
        Saturate,     // a = factor
        HueRotate,    // a = degrees
        Invert,       // a = amount [0..1]
        Opacity,      // a = amount [0..1]
        DropShadow,   // dx, dy, blur (px); shadowColor
    };
    Kind kind = Blur;
    float a = 0;
    float dx = 0;
    float dy = 0;
    float blur = 0;
    Color shadowColor = {0, 0, 0, 255};
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void clear(Color color) = 0;

    virtual void drawRect(float x, float y, float w, float h, Color color) = 0;
    virtual void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) = 0;
    virtual void fillRect(float x, float y, float w, float h, Color color) = 0;
    virtual void fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) = 0;

    // Per-corner asymmetric variants (CSS border-radius full support).
    // Default forwards to the symmetric path using the average corner radius
    // so backends can adopt incrementally.
    virtual void fillRoundRectRadii(float x, float y, float w, float h,
                                    const Radii& r, Color color) {
        float avg = (r.x[0] + r.x[1] + r.x[2] + r.x[3] +
                     r.y[0] + r.y[1] + r.y[2] + r.y[3]) / 8.0f;
        fillRoundRect(x, y, w, h, avg, avg, color);
    }
    virtual void drawRoundRectRadii(float x, float y, float w, float h,
                                    const Radii& r, float strokeWidth, Color color) {
        (void)strokeWidth;
        float avg = (r.x[0] + r.x[1] + r.x[2] + r.x[3] +
                     r.y[0] + r.y[1] + r.y[2] + r.y[3]) / 8.0f;
        drawRoundRect(x, y, w, h, avg, avg, color);
    }
    // Clip subsequent draw calls to a rounded rect (caller must save/restore).
    virtual void setClipRRect(float x, float y, float w, float h, const Radii& r) {
        (void)r;
        setClip(x, y, w, h);
    }
    // Box shadow with per-corner radii. Default forwards to symmetric path.
    virtual void drawBoxShadowRadii(float x, float y, float w, float h,
                                    const Radii& r,
                                    float offsetX, float offsetY,
                                    float blur, float spread,
                                    Color color, bool inset) {
        float avg = (r.x[0] + r.x[1] + r.x[2] + r.x[3] +
                     r.y[0] + r.y[1] + r.y[2] + r.y[3]) / 8.0f;
        drawBoxShadow(x, y, w, h, avg, avg, offsetX, offsetY, blur, spread, color, inset);
    }

    virtual void drawText(std::string_view text, float x, float y, FontRef font, Color color) = 0;
    virtual TextMetrics measureText(std::string_view text, FontRef font) = 0;

    // Extended text draw: letter-spacing applies a per-character advance on
    // top of the natural glyph advance (matching CSS letter-spacing). When
    // blur > 0 the glyphs are blurred — used to render text-shadow halos.
    // Default forwards to plain drawText so backends can adopt incrementally.
    virtual void drawTextEx(std::string_view text, float x, float y,
                            FontRef font, Color color,
                            float letterSpacing, float blur) {
        (void)letterSpacing; (void)blur;
        drawText(text, x, y, font, color);
    }

    // Register a custom font from file data (for @font-face support).
    // Returns true on success. The font will be used when a FontRef matches
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

    // Concatenate a 4x4 matrix in column-major form (16 floats; m[0..3] = col 0,
    // m[4..7] = col 1, ...). Used for CSS 3D transforms with perspective.
    virtual void concat4x4(const float m[16]) = 0;

    // Save a layer with a CSS filter chain applied. Everything drawn until the
    // matching restore() passes through the filter. The backend constructs the
    // native filter from the descriptor list. An empty span behaves like
    // save().
    virtual void saveLayerWithFilter(std::span<const CssFilterParams> filters,
                                     float x, float y, float w, float h) = 0;

    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void resetClip() = 0;

    // Clip subsequent draw calls to an arbitrary polygon (caller must save/restore).
    // Default implementation is a no-op so backends can adopt incrementally —
    // callers should still see un-clipped output rather than a crash.
    virtual void setClipPolygon(std::span<const PointF> /*points*/) {}

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
