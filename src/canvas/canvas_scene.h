#pragma once

#include "canvas/canvas2d.h"
#include "render/renderer.h"

#include <string>
#include <vector>
#include <unordered_map>

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>

#include <glad/gl.h>

namespace bro::render { class GLContext; }

namespace bro::canvas {

/// Per-canvas Skia-backed renderer.  Each CanvasScene owns an SkSurface and
/// draws directly via SkCanvas — no command buffer.  The raster pixels are
/// uploaded to a GL texture for compositing by the engine.
class CanvasScene {
public:
    explicit CanvasScene(render::Renderer* renderer);
    ~CanvasScene();

    CanvasScene(const CanvasScene&) = delete;
    CanvasScene& operator=(const CanvasScene&) = delete;

    // --- Layout / lifecycle callbacks (unchanged) ---

    using LayoutCallback = void(*)(void* userdata, float& outX, float& outY, float& outW, float& outH);
    void setLayoutCallback(LayoutCallback cb, void* ud) { layoutCb_ = cb; layoutUd_ = ud; }

    using DetachedCallback = bool(*)(void* userdata);
    void setDetachedCallback(DetachedCallback cb, void* ud) { detachedCb_ = cb; detachedUd_ = ud; }

    void init(render::GLContext* gl) { gl_ = gl; }
    void cleanup();

    render::Renderer* renderer() const { return renderer_; }
    int width() const { return queryLayoutWidth(); }
    int height() const { return queryLayoutHeight(); }
    SkSurface* surface() const { return surface_.get(); }

    void setViewportScroll(float scrollY) { viewportScrollY_ = scrollY; }
    bool isDetached() const { return detached_; }

    // --- Canvas 2D state setters (called from JS bindings) ---

    void setFillColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getFillColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    void setStrokeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getStrokeColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;

    void setLineWidth(float w);
    float lineWidth() const;

    void setGlobalAlpha(float a);
    float globalAlpha() const;

    void setLineCap(int cap);     // 0=butt, 1=round, 2=square
    int lineCap() const;
    void setLineJoin(int join);   // 0=miter, 1=round, 2=bevel
    int lineJoin() const;
    void setMiterLimit(float limit);
    float miterLimit() const;

    void setGlobalCompositeOperation(int op);
    int globalCompositeOperation() const;

    void setFont(const std::string& fontStr);
    const std::string& fontString() const { return fontString_; }

    void setTextAlign(int align);   // 0=start/left, 1=center, 2=right, 3=end
    int textAlign() const { return textAlign_; }
    void setTextBaseline(int bl);   // 0=alphabetic, 1=top, 2=middle, 3=bottom, 4=hanging, 5=ideographic
    int textBaseline() const { return textBaseline_; }

    void setShadowBlur(float blur);
    float shadowBlur() const { return shadowBlur_; }
    void setShadowColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void getShadowColor(uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const;
    void setShadowOffsetX(float x);
    float shadowOffsetX() const { return shadowOffsetX_; }
    void setShadowOffsetY(float y);
    float shadowOffsetY() const { return shadowOffsetY_; }

    void setImageSmoothingEnabled(bool v);
    bool imageSmoothingEnabled() const { return imageSmoothingEnabled_; }

    // --- Drawing methods ---

    void fillRect(float x, float y, float w, float h);
    void strokeRect(float x, float y, float w, float h);
    void clearRect(float x, float y, float w, float h);
    void fillText(const std::string& text, float x, float y);
    void strokeText(const std::string& text, float x, float y);
    render::TextMetrics measureText(const std::string& text);

    // --- Path API ---

    void beginPath();
    void moveTo(float x, float y);
    void lineTo(float x, float y);
    void closePath();
    void stroke();
    void fill();
    void clip();
    void arc(float cx, float cy, float radius, float startAngle, float endAngle, bool acw);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void ellipse(float cx, float cy, float rx, float ry, float rotation,
                 float startAngle, float endAngle, bool acw);
    void rect(float x, float y, float w, float h);
    bool isPointInPath(float x, float y);

    // --- Transform ---

    void save();
    void restore();
    void translate(float tx, float ty);
    void rotate(float angle);
    void scale(float sx, float sy);
    void setTransform(float a, float b, float c, float d, float e, float f);
    void resetTransform();
    void transform(float a, float b, float c, float d, float e, float f);

    // --- Image ---

    void drawImage(const void* rgbaData, int imgW, int imgH,
                   float sx, float sy, float sw, float sh,
                   float dx, float dy, float dw, float dh);

    // --- Pixel manipulation ---

    std::vector<uint8_t> getImageData(int x, int y, int w, int h);
    void putImageData(const uint8_t* data, int w, int h, int dx, int dy);

    // --- Reset (discard content) ---

    void reset();

    // --- Compositing support ---

    /// Ensure the backing surface matches the layout size and upload to GL.
    /// Call once per frame before compositing.
    void rasterize(render::GLContext* gl);

    GLuint texture() const { return glTexture_; }

    void getScreenRect(float& x, float& y, float& w, float& h) const {
        x = screenX_; y = screenY_;
        w = static_cast<float>(surfWidth_);
        h = static_cast<float>(surfHeight_);
    }

    /// Mark the canvas as dirty (needing GL texture re-upload).
    void markDirty() { dirty_ = true; }

private:
    int queryLayoutWidth() const;
    int queryLayoutHeight() const;
    void ensureSurface(int w, int h);
    SkCanvas* skCanvas();
    SkPaint makeFillPaint() const;
    SkPaint makeStrokePaint() const;
    void applyFont();
    void applyShadow(SkPaint& paint) const;
    float adjustTextX(float x, float textWidth) const;
    float adjustTextY(float y) const;

    render::Renderer* renderer_;
    render::GLContext* gl_ = nullptr;
    LayoutCallback layoutCb_ = nullptr;
    void* layoutUd_ = nullptr;
    DetachedCallback detachedCb_ = nullptr;
    void* detachedUd_ = nullptr;
    float viewportScrollY_ = 0;
    bool detached_ = false;

    // Skia surface (raster, RGBA premul)
    sk_sp<SkSurface> surface_;
    int surfWidth_ = 0, surfHeight_ = 0;

    // GL texture for compositing
    GLuint glTexture_ = 0;
    bool dirty_ = false;  // surface pixels changed, need GL re-upload

    // Screen-space position for compositing
    float screenX_ = 0, screenY_ = 0;

    // --- Canvas 2D state ---

    struct State {
        SkPaint fillPaint;
        SkPaint strokePaint;
        float lineWidthVal = 1.0f;
        float globalAlphaVal = 1.0f;
        int lineCapVal = 0;    // SkPaint::kButt_Cap
        int lineJoinVal = 0;   // SkPaint::kMiter_Join
        float miterLimitVal = 10.0f;
        int compositeOp = 0;   // source-over
        std::string fontStr = "16px sans-serif";
        int textAlignVal = 0;
        int textBaselineVal = 0;
        float shadowBlurVal = 0;
        uint8_t shadowR = 0, shadowG = 0, shadowB = 0, shadowA = 0;
        float shadowOX = 0, shadowOY = 0;
        bool imgSmooth = true;
    };

    State state_;
    std::vector<State> stateStack_;

    // Current path (built incrementally, snapshot()'d for drawing)
    SkPathBuilder pathBuilder_;

    // Current font
    SkFont font_;
    std::string fontString_ = "16px sans-serif";
    int textAlign_ = 0;
    int textBaseline_ = 0;
    float shadowBlur_ = 0;
    uint8_t shadowR_ = 0, shadowG_ = 0, shadowB_ = 0, shadowA_ = 0;
    float shadowOffsetX_ = 0, shadowOffsetY_ = 0;
    bool imageSmoothingEnabled_ = true;

    // Font cache (CSS string -> SkFont)
    struct FontCacheEntry {
        sk_sp<SkTypeface> typeface;
        SkFont font;
    };
    std::unordered_map<std::string, FontCacheEntry> fontCache_;
};

} // namespace bro::canvas
