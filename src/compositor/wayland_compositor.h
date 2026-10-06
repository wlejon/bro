#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Window and monitor snapshots are brocompositor's platform-neutral types; the
// Wayland server behind them is Linux-only.
#include <brocompositor/events.h>

#if BRO_HAVE_WAYLAND_SERVER
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
    bool xwayland = true;
    uint32_t width = 1280;
    uint32_t height = 720;
    std::string socketName = "";
    std::string renderNode = "";
};

struct LeasedSurfaceFrame {
    uint32_t surfaceId = 0;
#if BRO_HAVE_WAYLAND_SERVER
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
    std::string xwaylandDisplay() const;

    /// Pump and dispatch compositor events (server_events and wm events)
    void pollEvents();

    /// Acquire layers for currently mapped client surfaces to be composited by bro
    std::vector<LeasedSurfaceFrame> acquireClientLayers(std::vector<engine::UILayer>& outLayers);

    /// Release leased frames after presentation has completed
    void releaseClientLayers(const std::vector<LeasedSurfaceFrame>& frames);

    // Window management controls
    bool focusWindow(uint64_t winId);
    bool closeWindow(uint64_t winId);
    bool setWindowState(uint64_t winId, bool maximized, bool fullscreen);
    bool setWindowMinimized(uint64_t winId, bool minimized);
    bool placeWindow(uint64_t winId, int x, int y, int w, int h);
    std::vector<uint64_t> windows() const;
    std::optional<brocompositor::WindowSnapshot> queryWindow(uint64_t winId) const;

    // Multi-monitor & output management
    uint32_t addOutput(uint32_t width, uint32_t height);
    bool configureOutput(uint32_t outputId, float scale, int32_t x, int32_t y);
    std::vector<brocompositor::MonitorSnapshot> monitors() const;

    // Shell state queries
    bool isSessionLocked() const;

#if BRO_HAVE_WAYLAND_SERVER
    brocompositor::wl::ServerBackend* backend() { return backend_.get(); }
    brocompositor::WindowManager* windowManager() { return wm_.get(); }
    std::vector<brocompositor::wl::LayerSurfaceInfo> layerSurfaces() const;
#endif

private:
#if BRO_HAVE_WAYLAND_SERVER
    std::unique_ptr<brocompositor::wl::ServerBackend> backend_;
    std::unique_ptr<brocompositor::WindowManager> wm_;
#endif
    bool running_ = false;
    std::string socketName_;
};

} // namespace bro::compositor
