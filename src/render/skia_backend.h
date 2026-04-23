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
#include <include/core/SkFontStyle.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include "render/font_fallback.h"

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

    /// Create a standalone Ganesh GL GrDirectContext for the current thread's GL context.
    /// Returns nullptr if GPU initialization fails.
    static sk_sp<GrDirectContext> createGrContext();

    /// GPU Skia context (Ganesh GL) — nullptr if CPU-only mode.
    GrDirectContext* grContext() const { return grContext_.get(); }

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
    void drawPixelsRGBA(const uint8_t* rgba, int srcW, int srcH, int stride,
                        float x, float y, float w, float h) override;

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
    void rotate(float degrees) override;
    void concat(float a, float b, float c, float d, float e, float f) override;
    void saveLayerWithFilter(SkImageFilter* filter,
                             float x, float y, float w, float h) override;
    bool registerCustomFont(const std::string& family,
                            const void* data, size_t len,
                            int weight, bool italic) override;

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

    /// Switch the active drawing surface mid-frame (for compositing layers).
    /// Returns the previous surface. The new surface is cleared to transparent.
    /// Call between beginFrame() and endFrame().
    sk_sp<SkSurface> switchSurface(sk_sp<SkSurface> newSurface);

    /// Upload any Skia raster surface to a GL texture.
    /// Reuses existingTex if size matches, otherwise creates a new one.
    GLuint uploadSurfaceToTexture(SkSurface* surface, GLuint existingTex = 0);

    /// GPU-backed Skia surface (Ganesh) with its own FBO + GL texture.
    /// Used for HTML compositing layers so rendering goes directly to GPU
    /// with no CPU→GPU upload.
    struct GPUSurface {
        sk_sp<SkSurface> surface;
        GLuint texture = 0;
        GLuint fbo = 0;
    };

    /// Create a GPU-backed Skia surface at the given dimensions.
    GPUSurface createGPUSurface(int width, int height);

    /// Recreate the SkSurface wrapper for an existing FBO/texture.
    /// Cheap — only the Skia wrapper is recreated, GL resources stay alive.
    void rewrapGPUSurface(GPUSurface& surf, int width, int height);

    /// Destroy a GPU surface, releasing FBO and texture resources.
    void destroyGPUSurface(GPUSurface& surf);

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
        SkFontStyle style;
    };
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t next_font_handle_ = 1;

    // Persistent system font manager — shared by createFont and the font-
    // fallback path so per-glyph matchFamilyStyleCharacter lookups reuse it.
    sk_sp<SkFontMgr> fontMgr_;
    FontFallbackCache fallbackCache_;
    SkFontMgr* ensureFontMgr();

    // Custom font typefaces registered via @font-face
    struct CustomFont {
        std::string family;
        int weight;
        bool italic;
        sk_sp<SkTypeface> typeface;
    };
    std::vector<CustomFont> customFonts_;

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
