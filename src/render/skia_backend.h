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

#include "render/font_fallback.h"
#include "render/image_cache.h"
#include "render/shaped_run.h"
#include "render/skia_gpu.h"

#include <functional>

class GrDeferredDisplayListRecorder;

namespace bro::render {

// ---------------------------------------------------------------------------
// SkiaRenderer -- the UI (HTML/CSS) through Skia
//
// With a SkiaGpu (setGpu) its layer surfaces are GPU surfaces whose images
// VulkanPresenter composites in place; without one, CPU raster surfaces whose
// pixels it uploads. Drawing on a GPU surface is recorded into a deferred
// display list without the SkiaGpu lock — the raster thread replays a page
// while the main thread's canvases use the context — and only drawing that
// list into the surface, when drawing moves off it, takes the lock (briefly).
// ---------------------------------------------------------------------------

class SkiaRenderer final : public Renderer {
public:
    explicit SkiaRenderer();
    ~SkiaRenderer() override;

    void clear(bromath::Color color) override;

    void drawRect(float x, float y, float w, float h, bromath::Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color color) override;
    void fillRect(float x, float y, float w, float h, bromath::Color color) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, bromath::Color color) override;
    void fillRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, bromath::Color color) override;
    void drawRoundRectRadii(float x, float y, float w, float h,
                            const Radii& r, float strokeWidth, bromath::Color color) override;
    void setClipRRect(float x, float y, float w, float h, const Radii& r) override;
    void drawBoxShadowRadii(float x, float y, float w, float h, const Radii& r,
                            float offsetX, float offsetY,
                            float blur, float spread,
                            bromath::Color color, bool inset) override;

    void drawText(std::string_view text, float x, float y, FontRef font, bromath::Color color,
                  TextDirection direction = TextDirection::LTR) override;
    void drawTextEx(std::string_view text, float x, float y,
                    FontRef font, bromath::Color color,
                    float letterSpacing, float blur,
                    float wordSpacing = 0.0f,
                    TextDirection direction = TextDirection::LTR) override;
    TextMetrics measureText(std::string_view text, FontRef font,
                            TextDirection direction = TextDirection::LTR) override;

    void drawLine(float x1, float y1, float x2, float y2, bromath::Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h,
                   uint64_t imageId) override;
    void drawPixelsRGBA(const uint8_t* rgba, int srcW, int srcH, int stride,
                        float x, float y, float w, float h) override;
    void drawSharedPixels(const SharedPixels& px, float sx, float sy, float sw, float sh,
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
    void drawSvgPath(std::string_view d, PathFillRule rule,
                     const GradientPaint& fill, std::span<const ColorStop> fillStops,
                     const GradientPaint& stroke, std::span<const ColorStop> strokeStops,
                     const StrokeStyle& strokeStyle, std::span<const float> dash) override;
    void clipSvgPath(std::string_view d, PathFillRule rule) override;

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
    void drawBackdropFilter(std::span<const CssFilterParams> filters,
                            float x, float y, float w, float h, const Radii& r,
                            float opacity) override;
    bool registerCustomFont(const std::string& family,
                            const void* data, size_t len,
                            int weight, bool italic) override;

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

    /// Draw on the GPU through `gpu` from now on (null: the CPU). Before the
    /// first layer surface is made.
    void setGpu(SkiaGpu* gpu) { gpu_ = gpu; }
    SkiaGpu* skiaGpu() const override { return gpu_; }

    // beginFrame/endFrame bracket a frame of layer replays. endFrame finishes
    // the GPU surfaces drawn since beginFrame (their images ready to sample,
    // the work submitted), then runs the afterSubmit() callbacks.
    void beginFrame(int width, int height) override;
    void endFrame() override;

    /// Run `fn` once this frame's GPU work is submitted (at endFrame) — for
    /// handing an image to another thread only once it is ready to sample.
    void afterSubmit(std::function<void()> fn);

    /// Switch the active drawing surface mid-frame (for compositing layers).
    /// Returns the previous surface, with everything drawn on it applied (and,
    /// outside a frame, finished). The new surface is cleared to transparent.
    sk_sp<SkSurface> switchSurface(sk_sp<SkSurface> newSurface);

    /// Device pixels per recorded (CSS) unit for the surfaces entered from
    /// here on: beginFrame() and switchSurface() install it as the canvas's
    /// base matrix, below the base save() that resetClip() returns to, so a
    /// surface sized at scale × the CSS size is painted at full resolution
    /// (crisp text on a 2x display). 1 by default.
    void setDeviceScale(float scale) { deviceScale_ = scale > 0.0f ? scale : 1.0f; }
    float deviceScale() const { return deviceScale_; }

    /// A compositing layer's surface (HTML layers, system panels, iframe
    /// documents), drawn through switchSurface(): GPU with a SkiaGpu.
    using LayerSurface = render::LayerSurface;

    /// A surface of the given size.
    LayerSurface createLayerSurface(int width, int height);

    /// Make `surf` width x height, keeping its surface when it already is:
    /// switchSurface() clears it for the next frame.
    void fitLayerSurface(LayerSurface& surf, int width, int height);

    /// Release the surface (any thread).
    void releaseLayerSurface(LayerSurface& surf) { surf.reset(); }

    SkCanvas* getCanvas() const override { return canvas_; }
    SkSurface* surface() const override { return surface_.get(); }
    bool saveScreenshot(const std::string& path) override;
    std::vector<uint8_t> capturePixels() override;

private:
    SkColor toSkColor(bromath::Color c) const;

    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    float deviceScale_ = 1.0f;

    SkiaGpu* gpu_ = nullptr;
    // GPU surfaces drawn this frame, finished at endFrame (refs held so a
    // surface released mid-frame is still finished, then dropped under lock).
    std::vector<sk_sp<SkSurface>> touched_;
    bool inFrame_ = false;  // between beginFrame and endFrame
    // Records what is drawn on the current GPU surface (canvas_ is its canvas).
    std::unique_ptr<GrDeferredDisplayListRecorder> recorder_;
    // Draw the recorded list into surface_ (under the lock).
    void endRecording();
    // A cached decoded image as a texture, made once, so a recorded list
    // samples it rather than uploading it every time it is drawn. Dropped
    // (under the lock) with the decoded image's cache entry.
    struct GpuImage {
        sk_sp<SkImage> source;
        sk_sp<SkImage> texture;
        uint64_t lastFrame = 0;
    };
    std::unordered_map<uint64_t, GpuImage> gpuImages_;
    // Shared pixels (drawSharedPixels) by their id: the zero-copy raster
    // image and, drawing on the GPU, its texture, made once. Dropped with the
    // gpu images (and as they are), so the pixels are released ~a second
    // after they were last drawn.
    struct SharedImage {
        sk_sp<SkImage> raster;
        sk_sp<SkImage> texture;
        const uint8_t* rgba = nullptr;  // identity check: the id names these pixels
        uint64_t lastFrame = 0;
    };
    std::unordered_map<uint64_t, SharedImage> sharedImages_;
    uint64_t imageFrame_ = 0;
    sk_sp<SkImage> gpuImage(uint64_t id, const sk_sp<SkImage>& source);
    void evictGpuImages(bool all);
    std::vector<std::function<void()>> afterSubmit_;
    // Clear the current canvas and set it up for a frame: base matrix at the
    // device scale, then the base save().
    void enterCanvas();

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
    std::unordered_map<FontKey, FontEntry, FontKeyHash> fonts_;

    // Decoded-image cache — see DecodedImageCache. Swept once per beginFrame().
    DecodedImageCache imageCache_;

    // Resolve a FontRef to a cached FontEntry. Builds the SkFont on first miss
    // (consults customFonts_ + the platform font manager). Always returns a
    // pointer into the cache; the entry stays valid for the renderer's life.
    const FontEntry* getOrCreateFont(FontRef font);

    // Persistent system font manager — shared by font creation and the font-
    // fallback path so per-glyph matchFamilyStyleCharacter lookups reuse it.
    sk_sp<SkFontMgr> fontMgr_;
    FontFallbackCache fallbackCache_;
    SkFontMgr* ensureFontMgr();

public:
    const ShapedRun* shapeText(std::string_view text, FontRef font,
                               bool disableLigatures = false,
                               TextDirection direction = TextDirection::LTR) override;
    TextShapingEngine* textEngine() override { return &shaper_; }
    bool drawTextBlob(const SkTextBlob* blob, float x, float y,
                      bromath::Color color, float blur) override;

private:
    // Private and unshared, on the same footing as fonts_/fallbackCache_ —
    // that is what keeps the data plane lock-free here.
    TextShapingEngine shaper_;

    // Custom font typefaces registered via @font-face
    struct CustomFont {
        std::string family;
        int weight;
        bool italic;
        sk_sp<SkTypeface> typeface;
    };
    std::vector<CustomFont> customFonts_;
};

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
std::unique_ptr<Renderer> createRenderer();

} // namespace bro::render
