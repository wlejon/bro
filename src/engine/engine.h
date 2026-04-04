#pragma once

#include "engine/app_loader.h"
#include "engine/scrollbar.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "layout/font_manager.h"
#include <cstdint>
#include <memory>
#include <string>


namespace bro::render { class SceneLayer; class GLContext; class RasterRenderer; }
namespace broaudio { class Engine; }
namespace bro::audio { using AudioEngine = broaudio::Engine; }
namespace bro::engine { class SystemOverlay; }
namespace bro::canvas { class CanvasScene; }

namespace bro::platform {
    class Window;
    class EventLoop;
}
namespace bro::render { class Renderer; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; }
namespace bro::layout { class DrawTraversal; }

namespace bro::engine {

enum class DisplayMode { Windowed, Headless };

struct EngineConfig {
    std::string appDir;
    int width = 1024;
    int height = 768;
    DisplayMode displayMode = DisplayMode::Windowed;
    bool useGPU = true;  // headless uses GPU by default; --no-gpu disables
};

class Engine {
public:
    explicit Engine(const EngineConfig& config);
    ~Engine();

    /// Run the main event / render loop. Returns when the window is closed.
    /// In headless mode, performs initial layout and returns immediately.
    void run();

    /// Handle a window resize.
    void handleResize(int w, int h);

    /// Input events forwarded from the event loop.
    void handleMouseDown(float x, float y, int button);
    void handleMouseUp(float x, float y, int button);
    void handleMouseMove(float x, float y);
    void handleKeyDown(int keycode, int scancode, int mod, bool repeat);
    void handleKeyUp(int keycode, int scancode, int mod, bool repeat);
    void handleTextInput(const std::string& text);
    void handleWheel(float x, float y, float dx, float dy);

    // --- Headless API (also usable in windowed mode) ---

    /// Access the document.
    dom::Document* document() const { return document_.get(); }

    /// Access the renderer.
    render::Renderer* renderer() const { return renderer_.get(); }

    /// Access the JS runtime.
    js::Runtime* jsRuntime() const { return jsRuntime_.get(); }

    /// Access the timers.
    js::Timers* timers() const { return timers_.get(); }

    /// Access the system overlay.
    SystemOverlay* systemOverlay() const { return systemOverlay_.get(); }

    /// Run pending JS jobs and re-layout if dirty.
    void flush();

    /// Advance virtual time by the given milliseconds (headless mode).
    /// In windowed mode this is a no-op.
    void advanceTime(double ms);

    /// Evaluate JS code and return the string result.
    std::string eval(const std::string& code);

    /// Render the current page to a PNG file.
    bool screenshot(const std::string& path);

    /// Render the current page and crop to the given rect before saving.
    bool screenshot(const std::string& path, int x, int y, int w, int h);

    /// Capture the current page as an RGBA pixel buffer (width x height x 4).
    /// Returns empty vector on failure.
    std::vector<uint8_t> capturePixels();

    /// Find an element by selector (#id shorthand or CSS selector).
    dom::Element* querySelector(const std::string& selector) const;

    /// Simulate a click on the given element.
    void dispatchClickOn(dom::Element* target);

    /// Get display mode.
    DisplayMode displayMode() const { return displayMode_; }

    /// Get viewport dimensions.
    int viewportWidth() const { return viewportWidth_; }
    int viewportHeight() const { return viewportHeight_; }

    /// Get virtual time (headless mode).
    double virtualTime() const { return virtualTime_; }

private:
    dom::Element* hitTest(float x, float y);
    void dispatchEvent(dom::Element* target, dom::Event& event);
    void dispatchInputEvent(dom::Element* el, const std::string& data = "",
                            const std::string& inputType = "");
    void dispatchFocusEvents(dom::Element* oldTarget, dom::Element* newTarget);
    void dispatchScrollEvent(dom::Element* el);
    void advanceFocus(bool reverse);
    void addSceneLayer(std::unique_ptr<render::SceneLayer> layer);
    void ensureReplacedElements(dom::Element* elem);

    DisplayMode displayMode_;

    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<render::GLContext> gl_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<js::Runtime> jsRuntime_;
    std::unique_ptr<js::Timers> timers_;
    std::unique_ptr<dom::Document> document_;
    std::unique_ptr<layout::DrawTraversal> drawTraversal_;
    std::unique_ptr<layout::SkiaTextMetrics> textMetrics_;
    layout::FontManager fontManager_;
    std::unique_ptr<platform::EventLoop> eventLoop_;

    bool running_ = false;
    int viewportWidth_;
    int viewportHeight_;
    AppManifest manifest_;
    std::vector<std::unique_ptr<render::SceneLayer>> sceneLayers_;
    std::unique_ptr<audio::AudioEngine> audioEngine_;
    std::unique_ptr<SystemOverlay> systemOverlay_;

    // Headless-specific
    double virtualTime_ = 0.0;
    std::unique_ptr<canvas::CanvasScene> headlessCanvasScene_;
    canvas::CanvasScene* headlessCanvasScenePtr_ = nullptr;

    // Stats tracking
    double statsAccumMs_ = 0.0;
    int statsFrameCount_ = 0;
    double statsFps_ = 0.0;
    double statsFrameTimeMs_ = 0.0;
    double statsMinFrameMs_ = 999.0;
    double statsMaxFrameMs_ = 0.0;
    double totalFrameMs_ = 0.0;
    bool uiDirty_ = true;
    bool hasRenderedOnce_ = false;

    // Hover tracking for mouseenter/mouseleave/mouseover/mouseout
    dom::Element* hoveredElement_ = nullptr;

    // Mouse tracking for mousemove movement deltas
    float lastMouseX_ = 0.0f;
    float lastMouseY_ = 0.0f;

    // Double-click detection
    double lastClickTimeMs_ = 0.0;
    float lastClickX_ = 0.0f;
    float lastClickY_ = 0.0f;
    int clickCount_ = 0;
    dom::Element* lastClickTarget_ = nullptr;

    // Mouse button state for buttons bitmask
    int pressedButtons_ = 0;
    dom::Element* mouseDownTarget_ = nullptr;

    // Viewport scrolling
    float scrollY_ = 0.0f;
    float documentHeight_ = 0.0f;

    // Scrollbar components
    Scrollbar viewportScrollbar_;
    Scrollbar elementScrollbar_{Scrollbar::Style{5.0f, 1.0f, 16.0f,
        {255,255,255,20}, {255,255,255,100}, {255,255,255,150}, {255,255,255,180}}};
    bool draggingViewportScrollbar_ = false;
    dom::Element* scrollbarDragTarget_ = nullptr;
    dom::Element* scrollbarHoveredElement_ = nullptr;

    // UI render throttle — layout+rasterize at most every ~60fps
    static constexpr double kUIFrameIntervalMs = 8.0;
    double lastUIRenderMs_ = 0.0;

    // QuickJS cycle-collector GC — run periodically to free cyclic garbage
    static constexpr double kGCIntervalMs = 1000.0;
    double lastGCMs_ = 0.0;

    // Per-phase timing (smoothed over stats window)
    double phaseJsMs_ = 0.0;       // JS execution (rAF + pending jobs)
    double phaseLayoutMs_ = 0.0;   // layout
    double phaseRasterMs_ = 0.0;   // Skia rasterization + upload
    double phaseGpuMs_ = 0.0;      // GL composite + swap
    double phaseGlStateMs_ = 0.0;  // GL state save/restore
    // Raster sub-phase timing
    double phaseDrawMs_ = 0.0;     // draw (Skia commands)
    double phaseUploadMs_ = 0.0;   // texture upload to GPU
    // Accumulators for averaging
    double accumJsMs_ = 0.0;
    double accumLayoutMs_ = 0.0;
    double accumRasterMs_ = 0.0;
    double accumGpuMs_ = 0.0;
    double accumGlStateMs_ = 0.0;
    double accumDrawMs_ = 0.0;
    double accumUploadMs_ = 0.0;

    // UI overlay quad (OpenGL) — unsigned int to avoid including glad/gl.h
    unsigned int uiQuadVAO_ = 0;
    unsigned int uiQuadVBO_ = 0;
};

} // namespace bro::engine
