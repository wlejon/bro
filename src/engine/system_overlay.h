#pragma once

#include <memory>
#include <string>
#include <unordered_map>
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
#include "layout/font_manager.h"

namespace bro::render { class GLContext; }
namespace bro::layout { class DrawTraversal; }
namespace bro::dom { class Document; class Element; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::platform { class Window; }

namespace bro::engine {

class Settings;

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
    void drawBoxShadow(float x, float y, float w, float h,
                       float rx, float ry,
                       float offsetX, float offsetY,
                       float blur, float spread,
                       render::Color color, bool inset) override;
    void save() override;
    void restore() override;
    void saveLayerAlpha(uint8_t alpha) override;
    void translate(float dx, float dy) override;
    void scale(float sx, float sy) override;
    void setClip(float x, float y, float w, float h) override;
    void resetClip() override;
    void fillLinearGradient(float x, float y, float w, float h,
                            float startX, float startY, float endX, float endY,
                            std::span<const render::ColorStop> stops) override;
    void fillRadialGradient(float x, float y, float w, float h,
                            float cx, float cy, float rx, float ry,
                            std::span<const render::ColorStop> stops) override;
    void fillConicGradient(float x, float y, float w, float h,
                           float cx, float cy, float angleDeg,
                           std::span<const render::ColorStop> stops) override;
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
    SystemOverlay(js::Runtime* jsRuntime, render::GLContext* gl, int vpW, int vpH,
                  Settings* settings = nullptr, platform::Window* window = nullptr);
    ~SystemOverlay();

    /// Scan systemDir for subdirectories containing index.html and load each as a panel.
    /// Recurses into subdirectories (e.g. settings/graphics/).
    void loadPanels(const std::string& systemDir);

    /// Toggle perf overlay (F8).
    void togglePerf();
    /// Toggle settings menu (Esc).
    void toggleSettings();
    /// True if any overlay content is visible.
    bool isVisible() const { return perfVisible_ || settingsVisible_; }
    bool isPerfVisible() const { return perfVisible_; }
    bool isSettingsVisible() const { return settingsVisible_; }

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

    // --- Mouse event forwarding (Phase 1) ---

    /// Forward mouse events to overlay panel DOMs. Returns true if consumed.
    bool handleMouseDown(float x, float y, int button);
    bool handleMouseUp(float x, float y, int button);
    bool handleMouseMove(float x, float y);

    // --- Panel visibility control (Phase 2) ---

    /// Show a panel by name (e.g. "settings/graphics"). Hides other panels in
    /// the same group. Panels with group="" are always visible.
    void showPanel(const std::string& name);

    /// Get the name of the active settings panel.
    const std::string& getActivePanel() const { return activePanel_; }

    /// Get list of panel names and tab labels (for nav panel).
    struct PanelInfo { std::string name; std::string tabLabel; };
    std::vector<PanelInfo> getPanelList() const;

    // --- Overlay DOM inspection (headless) ---

    /// Query an element in an overlay panel's DOM.
    /// panelName: e.g. "nav", "perf", "settings/graphics"
    /// selector: CSS selector or #id
    dom::Element* querySelector(const std::string& panelName,
                                const std::string& selector);

    /// Get all panel names.
    std::vector<std::string> getPanelNames() const;

    struct Panel {
        std::string name;
        std::string tabLabel;   // display label for nav bar (empty = hidden from nav)
        std::string group;      // panels in the same group are mutually exclusive ("" = always visible)
        bool active = true;     // whether this panel is currently shown
        JSContext* jsCtx = nullptr;
        std::unique_ptr<js::Timers> timers;
        std::unique_ptr<layout::DrawTraversal> drawTraversal;
        std::unique_ptr<layout::FontManager> fontManager;
        std::unique_ptr<dom::Document> document;
        JSValue broPerfObj = JS_UNDEFINED;  // cached ref for fast updates
    };

private:
    void installBroObject(Panel& panel);
    void scanPanelDir(const std::string& baseDir, const std::string& relPath);

    /// Hit-test a single panel's DOM. Returns the deepest element hit, or nullptr.
    dom::Element* hitTestPanel(Panel& panel, float x, float y);

    /// Check if a panel should currently render.
    bool isPanelVisible(const Panel& panel) const;

    js::Runtime* jsRuntime_;  // shared, not owned
    render::GLContext* gl_;
    Settings* settings_ = nullptr;       // not owned
    platform::Window* window_ = nullptr; // not owned
    std::unique_ptr<SystemRenderer> renderer_;
    int viewportWidth_;
    int viewportHeight_;
    bool perfVisible_ = false;
    bool settingsVisible_ = false;
    bool renderDirty_ = true;  // true when panels need re-rasterize + upload
    std::string activePanel_;  // name of the currently visible settings panel

    // Mouse tracking for overlay panels
    dom::Element* overlayHoverTarget_ = nullptr;
    Panel* overlayHoverPanel_ = nullptr;

    std::vector<Panel> panels_;
};

} // namespace bro::engine
