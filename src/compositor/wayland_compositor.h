#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
struct ClientWindowRef;
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

/// A window as the shell host composites it: shown windows only, in stacking
/// order (stack(): bottom to top).
struct ClientWindowInfo {
    uint64_t id = 0;
    brocompositor::Rect frame;      // the client, layout px
    brocompositor::Margins insets;  // the shell frame's reach around it
    bool framed = false;            // the shell frames it (zero insets: borderless)
    bool focused = false;
    bool maximized = false;
    bool fullscreen = false;
    std::string snap;               // "none", "left", "right", "top-left", ...
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

    /// Pump and dispatch compositor events (server_events and wm events). Returns true if events occurred.
    bool pollEvents();

    /// Acquire layers for every mapped client surface, bottom to top: layer
    /// shell background / bottom, the windows in stacking order, override-
    /// redirect X11 surfaces, layer shell top / overlay (or, while the
    /// session is locked, only the lock surfaces).
    std::vector<LeasedSurfaceFrame> acquireClientLayers(std::vector<engine::UILayer>& outLayers);

    /// The same for one run of windows the shell interleaves with its own
    /// frames (render::ClientWindowsLayerSource): `parts` adds the layers
    /// below the windows (kClientLayersBelow) and above them
    /// (kClientLayersAbove). A pinned window is placed at its recorded
    /// client origin, any other where the compositor has it now.
    std::vector<LeasedSurfaceFrame> acquireClientLayers(std::span<const render::ClientWindowRef> windows,
                                                        uint32_t parts, std::vector<engine::UILayer>& outLayers);

    /// Shown windows, bottom to top (the window manager's stacking order).
    std::vector<ClientWindowInfo> stack() const;

    /// The flip that showed a frame, for the clients' presentation feedback.
    struct FramePresentation {
        int64_t timestampNs = 0;  // CLOCK_MONOTONIC
        uint64_t sequence = 0;    // the CRTC's vblank counter
        uint32_t refreshNs = 0;
    };

    /// Release leased frames after presentation has completed. `shown`, when
    /// known, is the flip the frame landed on.
    void releaseClientLayers(const std::vector<LeasedSurfaceFrame>& frames,
                             const FramePresentation* shown = nullptr);
    /// The two halves of that: the clients hear their frames were shown
    /// (presentation feedback, frame callbacks), and the buffers go back. A
    /// buffer scanned out directly is shown at one flip and goes back at the
    /// next, once it has left the screen.
    void notifyClientLayersShown(const std::vector<LeasedSurfaceFrame>& frames, const FramePresentation* shown);
    void returnClientLayers(const std::vector<LeasedSurfaceFrame>& frames);

    // Window management controls
    bool focusWindow(uint64_t winId);
    bool closeWindow(uint64_t winId);
    bool setWindowState(uint64_t winId, bool maximized, bool fullscreen);
    bool setWindowMinimized(uint64_t winId, bool minimized);
    bool placeWindow(uint64_t winId, int x, int y, int w, int h);
    std::vector<uint64_t> windows() const;
    std::optional<brocompositor::WindowSnapshot> queryWindow(uint64_t winId) const;
    uint64_t focusedWindow() const;

    // Interactive window move & resize
    bool startInteractiveMove(uint64_t winId, double startX, double startY, bool immediate = true);
    bool startInteractiveResize(uint64_t winId, double startX, double startY, uint32_t edges, bool immediate = true);
    bool updateInteractiveDrag(double curX, double curY);
    void endInteractiveDrag();
    bool isDraggingWindow() const;
    bool isDragActive() const;
    uint64_t draggedWindow() const;

    // Multi-monitor & output management
    uint32_t addOutput(uint32_t width, uint32_t height);
    bool configureOutput(uint32_t outputId, float scale, int32_t x, int32_t y);
    std::vector<brocompositor::MonitorSnapshot> monitors() const;

    // Shell state queries
    bool isSessionLocked() const;

    // Input injection
    void injectKey(uint32_t keycode, bool pressed);
    void injectPointerMotion(double dx, double dy);
    /// The pointer at (x, y); a device delta (dx, dy) also reaches the
    /// pointer-focused client as relative motion.
    void injectPointerWarp(double x, double y, double dx = 0.0, double dy = 0.0);
    void injectPointerButton(uint32_t button, bool pressed);
    void injectPointerAxis(uint32_t orientation, double delta, int32_t discrete = 0);

    /// The constraint the pointer-focused client holds on the pointer
    /// (zwp_pointer_constraints_v1), as of the last pollEvents.
    enum class PointerConstraint { None, Locked, Confined };
    PointerConstraint pointerConstraint() const { return pointerConstraint_; }
    /// Where the pointer was last put (injectPointerWarp), layout px.
    double pointerX() const { return lastPointerX_; }
    double pointerY() const { return lastPointerY_; }

#if BRO_HAVE_WAYLAND_SERVER
    brocompositor::wl::ServerBackend* backend() { return backend_.get(); }
    brocompositor::WindowManager* windowManager() { return wm_.get(); }
    std::shared_ptr<brocompositor::WindowManager> windowManagerShared() const { return wm_; }
    std::vector<brocompositor::wl::LayerSurfaceInfo> layerSurfaces() const;

    /// Current cursor state requested by Wayland clients or compositor
    brocompositor::wl::CursorChanged cursor() const;

    /// Hit-test Wayland windows at coordinates (x, y): the topmost window
    /// whose surfaces are under the point, in stacking order.
    uint64_t windowAt(double x, double y) const;
    /// Whether a surface of window winId is under (x, y).
    bool windowSurfaceAt(uint64_t winId, double x, double y) const;
    /// Whether an override-redirect X11 surface (a menu) is under (x, y).
    bool unmanagedAt(double x, double y) const;

    /// Route pointer to surface under (x, y), returns true if a Wayland surface was hit
    bool routePointer(double x, double y, uint32_t time = 0);

    /// The window manager's reading of a press at layout point (x, y) on
    /// window winId under its interaction policy (bro.compositor.setInteraction).
    brocompositor::PressDecision classifyPress(uint64_t winId, double x, double y, uint32_t modifiers,
                                               brocompositor::PressButton button) const;
#endif

private:
#if BRO_HAVE_WAYLAND_SERVER
    std::unique_ptr<brocompositor::wl::ServerBackend> backend_;
    std::shared_ptr<brocompositor::WindowManager> wm_;
#endif
    bool running_ = false;
    std::string socketName_;

    double lastPointerX_ = 0.0;
    double lastPointerY_ = 0.0;
    PointerConstraint pointerConstraint_ = PointerConstraint::None;
};

} // namespace bro::compositor
