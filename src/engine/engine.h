#pragma once

#include "engine/app_loader.h"
#include <litehtml.h>
#include <cstdint>
#include <memory>
#include <string>

#include <glad/gl.h>

namespace bro::render { class SceneLayer; class GLContext; }
namespace bro::audio { class AudioEngine; }

namespace bro::platform {
    class Window;
    class EventLoop;
}
namespace bro::render { class Renderer; }
namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; }
namespace bro::layout { class BroContainer; }

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

private:
    dom::Element* hitTest(float x, float y);
    void dispatchEvent(dom::Element* target, dom::Event& event);
    void setSceneLayer(std::unique_ptr<render::SceneLayer> layer);
    void drawStatsOverlay(double frameTimeMs);

    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<render::GLContext> gl_;
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<js::Runtime> jsRuntime_;
    std::unique_ptr<js::Timers> timers_;
    std::unique_ptr<dom::Document> document_;
    std::unique_ptr<layout::BroContainer> container_;
    std::unique_ptr<platform::EventLoop> eventLoop_;
    litehtml::document::ptr litehtmlDoc_;

    bool running_ = false;
    int viewportWidth_;
    int viewportHeight_;
    AppManifest manifest_;
    std::unique_ptr<render::SceneLayer> sceneLayer_;
    std::unique_ptr<audio::AudioEngine> audioEngine_;

    // Stats overlay
    uint64_t statsFont_ = 0;
    double statsAccumMs_ = 0.0;
    int statsFrameCount_ = 0;
    double statsFps_ = 0.0;
    double statsFrameTimeMs_ = 0.0;
    double statsMinFrameMs_ = 999.0;
    double statsMaxFrameMs_ = 0.0;
    double totalFrameMs_ = 0.0;
    bool uiDirty_ = true;
    bool hasRenderedOnce_ = false;

    // UI render throttle — layout+rasterize at most every ~60fps
    static constexpr double kUIFrameIntervalMs = 83.0;
    double lastUIRenderMs_ = 0.0;

    // Per-phase timing (smoothed over stats window)
    double phaseJsMs_ = 0.0;       // JS execution (rAF + pending jobs)
    double phaseLayoutMs_ = 0.0;   // litehtml layout
    double phaseRasterMs_ = 0.0;   // Skia rasterization + upload
    double phaseGpuMs_ = 0.0;      // GL composite + swap
    double phaseGlStateMs_ = 0.0;  // GL state save/restore
    // Accumulators for averaging
    double accumJsMs_ = 0.0;
    double accumLayoutMs_ = 0.0;
    double accumRasterMs_ = 0.0;
    double accumGpuMs_ = 0.0;
    double accumGlStateMs_ = 0.0;

    // UI overlay quad (OpenGL)
    GLuint uiQuadVAO_ = 0;
    GLuint uiQuadVBO_ = 0;
};

} // namespace bro::engine
