#pragma once

#include "engine/app_loader.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "layout/font_manager.h"
#include <cstdint>
#include <memory>
#include <string>

#include <glad/gl.h>

namespace bro::render { class SceneLayer; class GLContext; }
namespace bro::audio { class AudioEngine; }
namespace bro::engine { class SystemOverlay; }

namespace bro::platform {
    class Window;
    class EventLoop;
}
namespace bro::render { class Renderer; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; }
namespace bro::layout { class DrawTraversal; }

namespace bro::engine {

class Engine {
public:
    explicit Engine(const std::string& appDir, int width = 1024, int height = 768);
    ~Engine();

    /// Run the main event / render loop. Returns when the window is closed.
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

private:
    dom::Element* hitTest(float x, float y);
    void dispatchEvent(dom::Element* target, dom::Event& event);
    void dispatchInputEvent(dom::Element* el);
    void advanceFocus(bool reverse);
    void setSceneLayer(std::unique_ptr<render::SceneLayer> layer);

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
    std::unique_ptr<render::SceneLayer> sceneLayer_;
    std::unique_ptr<audio::AudioEngine> audioEngine_;
    std::unique_ptr<SystemOverlay> systemOverlay_;

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

    // Viewport scrolling
    float scrollY_ = 0.0f;
    float documentHeight_ = 0.0f;
    static constexpr float kScrollSpeed = 48.0f; // pixels per wheel tick
    static constexpr int kScrollbarWidth = 8;
    static constexpr int kScrollbarMargin = 2;

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

    // UI overlay quad (OpenGL)
    GLuint uiQuadVAO_ = 0;
    GLuint uiQuadVBO_ = 0;
};

} // namespace bro::engine
