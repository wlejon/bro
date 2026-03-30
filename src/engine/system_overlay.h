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
namespace bro::js { class Timers; }

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
    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;
    void beginFrame(int width, int height) override;
    void endFrame() override;

    /// Upload rasterized pixels to GL texture.
    void uploadToGPU();

    /// Get the overlay GL texture.
    GLuint getTexture() const { return texture_; }

    /// Debug: get canvas pointer.
    SkCanvas* canvas() const { return canvas_; }

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
    SystemOverlay(render::GLContext* gl, int vpW, int vpH);
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

    /// Handle viewport resize.
    void onResize(int w, int h);

    struct Panel {
        std::string name;
        litehtml::document::ptr litehtmlDoc;
        std::unique_ptr<layout::BroContainer> container;
        std::unique_ptr<dom::Document> document;
    };

private:
    void installMinimalBindings();
    void installBroObject();

    render::GLContext* gl_;
    std::unique_ptr<SystemRenderer> renderer_;
    int viewportWidth_;
    int viewportHeight_;
    bool visible_ = false;

    // Own JS environment (isolated from the app)
    JSRuntime* jsRt_ = nullptr;
    JSContext* jsCtx_ = nullptr;
    std::unique_ptr<js::Timers> timers_;

    // __bro.perf JS object references (for fast property updates)
    JSValue broPerfObj_ = JS_UNDEFINED;

    std::vector<Panel> panels_;
};

} // namespace bro::engine
