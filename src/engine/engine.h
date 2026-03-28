#pragma once

#include "engine/app_loader.h"
#include <litehtml.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bro::platform {
    class Window;
    class VulkanContext;
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
    void handleKeyDown(int keycode, int scancode, int mod);
    void handleKeyUp(int keycode, int scancode, int mod);

private:
    dom::Element* hitTest(float x, float y);
    void dispatchEvent(dom::Element* target, dom::Event& event);
    void drawStatsOverlay(double frameTimeMs);

    std::unique_ptr<platform::Window> window_;
    std::unique_ptr<platform::VulkanContext> vulkan_;
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

    // Stats overlay
    uint64_t statsFont_ = 0;
    double statsAccumMs_ = 0.0;
    int statsFrameCount_ = 0;
    double statsFps_ = 0.0;
    double statsFrameTimeMs_ = 0.0;
    double statsMinFrameMs_ = 999.0;
    double statsMaxFrameMs_ = 0.0;
};

} // namespace bro::engine
