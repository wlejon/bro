#pragma once

#include "render/renderer.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFont.h>
#include <include/core/SkTypeface.h>
#include <include/core/SkFontMgr.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <glad/gl.h>

namespace bro::render {

class GLContext;

// ---------------------------------------------------------------------------
// SkiaRenderer -- Skia raster UI + OpenGL display
//
// The UI (HTML/CSS) is rendered to a CPU-side Skia surface with transparency,
// uploaded to an OpenGL texture, and composited over GPU-rendered scene
// content via the texture pipeline.
// ---------------------------------------------------------------------------

class SkiaRenderer final : public Renderer {
public:
    explicit SkiaRenderer(GLContext& gl);
    ~SkiaRenderer() override;

    void clear(Color color) override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;
    void fillRect(float x, float y, float w, float h, Color color) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, Color color) override;

    void drawText(std::string_view text, float x, float y, uint64_t font_handle, Color color) override;
    TextMetrics measureText(std::string_view text, uint64_t font_handle) override;

    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t font_handle) override;

    void drawLine(float x1, float y1, float x2, float y2, Color color, float thickness) override;
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

    // beginFrame/endFrame manage the Skia raster surface for UI rendering.
    void beginFrame(int width, int height) override;
    void endFrame() override;

    /// Upload Skia pixels to the GL texture. Call after endFrame().
    void uploadToGPU();

    /// Access the UI overlay GL texture (BGRA8, premultiplied alpha).
    GLuint getUITexture() const { return uiTexture_; }

    /// Render text to a GL texture (for scene-layer text).
    /// Caller does NOT own the texture — it is cached internally.
    GLuint renderTextToTexture(std::string_view text, uint64_t font_handle,
                               Color color, int& outW, int& outH);

    GLContext* gl() const { return gl_; }

    SkCanvas* getCanvas() const override { return canvas_; }
    SkSurface* surface() const override { return surface_.get(); }
    bool saveScreenshot(const std::string& path) override;
    std::vector<uint8_t> capturePixels() override;

private:
    SkColor toSkColor(Color c) const;

    GLContext* gl_ = nullptr;
    GLuint uiTexture_ = 0;
    int textureWidth_ = 0;
    int textureHeight_ = 0;

    // Skia GPU context (Ganesh GL backend)
    sk_sp<GrDirectContext> grContext_;
    GLuint gpuFBO_ = 0;         // FBO that wraps uiTexture_ for Skia GPU rendering

    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    bool gpuMode_ = false;      // true if GPU backend active

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t next_font_handle_ = 1;

    // Cached text textures for scene-layer rendering
    struct TextCacheEntry { GLuint tex; int w; int h; };
    std::unordered_map<std::string, TextCacheEntry> textTexCache_;

    // Pending pixel data for upload
    bool pixelsPending_ = false;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer(GLContext* gl);

} // namespace bro::render
