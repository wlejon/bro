#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
#include <brocompositor/linux/server.h>
#include <brocompositor/surface.h>
#include <brocompositor/window_manager.h>
#endif

namespace bro::render {
struct DmabufLayerSource;
}

namespace bro::engine {
struct UILayer;
}

namespace bro::compositor {

struct CompositorConfig {
    bool headless = true;
    bool drm = false;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string socketName = "";
    std::string renderNode = "";
};

struct LeasedSurfaceFrame {
    uint32_t surfaceId = 0;
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    std::shared_ptr<brocompositor::wl::ClientSurface> surface;
    brocompositor::Frame frame;
#endif
};

class WaylandCompositor {
public:
    WaylandCompositor();
    ~WaylandCompositor();

    WaylandCompositor(const WaylandCompositor&) = delete;
    WaylandCompositor& operator=(const WaylandCompositor&) = delete;

    bool init(const CompositorConfig& config, std::string* error = nullptr);
    void shutdown();

    bool isRunning() const;
    std::string socketName() const;

    /// Pump and dispatch compositor events (server_events and wm events)
    void pollEvents();

    /// Acquire layers for currently mapped client surfaces to be composited by bro
    std::vector<LeasedSurfaceFrame> acquireClientLayers(std::vector<engine::UILayer>& outLayers);

    /// Release leased frames after presentation has completed
    void releaseClientLayers(const std::vector<LeasedSurfaceFrame>& frames);

#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    brocompositor::wl::ServerBackend* backend() { return backend_.get(); }
    brocompositor::WindowManager* windowManager() { return wm_.get(); }
#endif

private:
#if defined(__linux__) && defined(BRO_WITH_COMPOSITOR)
    std::unique_ptr<brocompositor::wl::ServerBackend> backend_;
    std::unique_ptr<brocompositor::WindowManager> wm_;
#endif
    bool running_ = false;
    std::string socketName_;
};

} // namespace bro::compositor
