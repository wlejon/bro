#pragma once

#include <litehtml.h>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#include <include/core/SkFont.h>
#include <include/core/SkTypeface.h>

extern "C" {
#include "quickjs.h"
}

#include "render/renderer.h"

namespace bro::render { class GLContext; }
namespace bro::layout { class BroContainer; }
namespace bro::dom { class Document; }
namespace bro::js { class Runtime; class Timers; }

namespace bro::engine {

/// CPU-raster Renderer for system overlay panels.
/// Uses a CPU SkSurface and uploads pixels to a GL texture for compositing.
class SystemRenderer : public render::Renderer {
public:
    explicit SystemRenderer(render::GLContext* gl);
    ~SystemRenderer() override;

    void clear(render::Color color) override;
    void drawRect(float x, float y, float w, float h, render::Color color) override;
    void drawRoundRect(float x, float y, float w, float h, float rx, float ry, render::Color color) override;
    void fillRect(float x, float y, float w, float h, render::Color color) override;
    void drawText(std::string_view text, float x, float y, uint64_t font_handle, render::Color color) override;
    render::TextMetrics measureText(std::string_view text, uint64_t font_handle) override;
    uint64_t createFont(std::string_view family, float size, int weight, bool italic) override;
    void deleteFont(uint64_t font_handle) override;
    void drawLine(float x1, float y1, float x2, float y2, render::Color color, float thickness) override;
    void drawImage(const void* data, size_t len, float x, float y, float w, float h) override;
    void fillRoundRect(float x, float y, float w, float h, float rx, float ry, render::Color color) override;
    void drawCircle(float cx, float cy, float r,
                    render::Color fill, render::Color stroke, float strokeWidth) override;
    void drawEllipse(float cx, float cy, float rx, float ry,
                     render::Color fill, render::Color stroke, float strokeWidth) override;
    void drawPath(std::string_view svgPathData,
                  render::Color fill, render::Color stroke, float strokeWidth) override;
    void drawPolygon(std::span<const render::PointF> points,
                     render::Color fill, render::Color stroke, float strokeWidth) override;
    void drawPolyline(std::span<const render::PointF> points,
                      render::Color stroke, float strokeWidth) override;
    void save() override;
    void restore() override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;
    void beginFrame(int width, int height) override;
    void endFrame() override;

    /// Upload rasterized pixels to GL texture (no-op if no GLContext).
    void uploadToGPU();

    /// Get the overlay GL texture.
    GLuint getTexture() const { return texture_; }

    /// Get the Skia surface (for headless screenshots).
    SkSurface* surface() const { return surface_.get(); }

private:
    SkColor toSkColor(render::Color c) const;

    render::GLContext* gl_;
    sk_sp<SkSurface> surface_;
    SkCanvas* canvas_ = nullptr;
    GLuint texture_ = 0;
    int texWidth_ = 0;
    int texHeight_ = 0;
    int baseSaveCount_ = 0;

    struct FontEntry {
        sk_sp<SkTypeface> typeface;
        std::unique_ptr<SkFont> font;
    };
    std::unordered_map<uint64_t, FontEntry> fonts_;
    uint64_t nextFontHandle_ = 1;
};

class SystemOverlay {
public:
    /// Construct with shared JS runtime. Each panel gets its own JSContext.
    SystemOverlay(js::Runtime* jsRuntime, render::GLContext* gl, int vpW, int vpH);
    ~SystemOverlay();

    /// Scan systemDir for subdirectories containing index.html and load each as a panel.
    void loadPanels(const std::string& systemDir);

    /// Toggle overlay visibility.
    void toggle();
    bool isVisible() const { return visible_; }

    /// Update performance data (called each stats accumulation cycle).
    void updatePerf(double fps, double frameTime, double js, double layout,
                    double raster, double gpu, double draw, int vpW, int vpH);

    /// Tick JS timers and run pending jobs.
    void tick(double nowMs);

    /// Rasterize all visible panels to own surface + upload to GL texture.
    void render(int vpW, int vpH);

    /// Get the GL texture containing the rendered overlay (premultiplied alpha).
    GLuint getTexture() const;

    /// Get the renderer (for headless screenshot compositing).
    SystemRenderer* getRenderer() const { return renderer_.get(); }

    /// Handle viewport resize.
    void onResize(int w, int h);

    struct Panel {
        std::string name;
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        litehtml::document::ptr litehtmlDoc;
        std::unique_ptr<layout::BroContainer> container;
        std::unique_ptr<dom::Document> document;
        JSValue broPerfObj = JS_UNDEFINED;  // cached ref for fast updates
    };

private:
    void installBroObject(Panel& panel);

    js::Runtime* jsRuntime_;  // shared, not owned
    render::GLContext* gl_;
    std::unique_ptr<SystemRenderer> renderer_;
    int viewportWidth_;
    int viewportHeight_;
    bool visible_ = false;
    bool renderDirty_ = true;  // true when panels need re-rasterize + upload

    std::vector<Panel> panels_;
};

} // namespace bro::engine
