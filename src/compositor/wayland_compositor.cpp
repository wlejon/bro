#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"

namespace bro::compositor {

WaylandCompositor::WaylandCompositor() = default;

WaylandCompositor::~WaylandCompositor() {
    shutdown();
}

bool WaylandCompositor::init(const CompositorConfig& config, std::string* error) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    brocompositor::wl::ServerConfig sCfg;
    if (config.headless) {
        sCfg.backend = brocompositor::wl::BackendKind::Headless;
    } else if (config.drm) {
        sCfg.backend = brocompositor::wl::BackendKind::Drm;
    } else {
        sCfg.backend = brocompositor::wl::BackendKind::Auto;
    }
    sCfg.xwayland = config.xwayland ? brocompositor::wl::XwaylandMode::Lazy : brocompositor::wl::XwaylandMode::Off;
    sCfg.initial_outputs = 1;
    sCfg.initial_output_size = {static_cast<int32_t>(config.width), static_cast<int32_t>(config.height)};
    sCfg.socket_name = config.socketName;
    sCfg.render_node = config.renderNode;

    backend_ = brocompositor::wl::ServerBackend::create(sCfg, error);
    if (!backend_) return false;

    wm_ = std::make_unique<brocompositor::WindowManager>();
    socketName_ = backend_->socket_name();
    running_ = true;
    return true;
#else
    if (error) *error = "brocompositor is not available on this platform or build";
    return false;
#endif
}

void WaylandCompositor::shutdown() {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    wm_.reset();
    backend_.reset();
#endif
    running_ = false;
    socketName_.clear();
}

bool WaylandCompositor::isRunning() const {
    return running_;
}

std::string WaylandCompositor::socketName() const {
    return socketName_;
}

std::string WaylandCompositor::xwaylandDisplay() const {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (backend_) {
        return backend_->xwayland_display();
    }
#endif
    return {};
}

void WaylandCompositor::pollEvents() {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return;

    auto events = backend_->events().drain();
    for (auto& ev : events) {
        if (wm_) {
            auto cmd = wm_->handle(ev);
            backend_->execute(cmd);
        }
    }

    auto sevents = backend_->server_events().drain();
    (void)sevents;
#endif
}

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::vector<engine::UILayer>& outLayers) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return {};

    std::vector<LeasedSurfaceFrame> leasedFrames;

    auto appendSurfaceNode = [&](const brocompositor::wl::SurfaceNode& node, float baseX, float baseY) {
        auto surface = backend_->surface(node.surface);
        if (!surface) return;

        auto frameOpt = surface->acquire();
        if (!frameOpt) return;

        auto imgOpt = surface->image(frameOpt->image_id);
        if (!imgOpt) {
            surface->release(*frameOpt);
            return;
        }

        if (imgOpt->type == brocompositor::ImageHandleType::DmaBuf) {
            render::DmabufLayerSource dmabufSrc;
            dmabufSrc.bufferId = imgOpt->id;
            dmabufSrc.width = imgOpt->width;
            dmabufSrc.height = imgOpt->height;
            dmabufSrc.drmFormat = imgOpt->drm_format;
            dmabufSrc.modifier = imgOpt->drm_modifier;
            dmabufSrc.planeCount = static_cast<uint32_t>(imgOpt->planes.size());
            for (size_t p = 0; p < imgOpt->planes.size() && p < 4; ++p) {
                dmabufSrc.fds[p] = brocompositor::wl::fd_of(imgOpt->planes[p].handle);
                dmabufSrc.strides[p] = imgOpt->planes[p].stride;
                dmabufSrc.offsets[p] = imgOpt->planes[p].offset;
            }
            dmabufSrc.syncFd = brocompositor::wl::fd_of(frameOpt->sync_fd);

            engine::UILayer layer;
            layer.quad.x = baseX + static_cast<float>(node.offset.x);
            layer.quad.y = baseY + static_cast<float>(node.offset.y);
            layer.quad.w = static_cast<float>(imgOpt->width);
            layer.quad.h = static_cast<float>(imgOpt->height);
            layer.quad.clipW = -1.0f;
            layer.content = dmabufSrc;

            outLayers.push_back(layer);

            LeasedSurfaceFrame leased;
            leased.surfaceId = static_cast<uint32_t>(node.surface);
            leased.surface = surface;
            leased.frame = *frameOpt;
            leasedFrames.push_back(std::move(leased));
        } else {
            surface->release(*frameOpt);
        }
    };

    // 1. Session lock: if locked, ONLY lock surfaces are visible
    if (backend_->session_lock_state() != brocompositor::wl::LockState::Unlocked) {
        for (const auto& mon : backend_->monitors()) {
            for (const auto& node : backend_->lock_surface_tree(mon.id)) {
                appendSurfaceNode(node, static_cast<float>(mon.bounds.x), static_cast<float>(mon.bounds.y));
            }
        }
        return leasedFrames;
    }

    // 2. Background layer surfaces (e.g. wallpaper)
    for (const auto& ls : backend_->layer_surfaces()) {
        if (!ls.mapped || ls.layer != brocompositor::wl::Layer::Background) continue;
        for (const auto& node : backend_->layer_surface_tree(ls.id)) {
            appendSurfaceNode(node, static_cast<float>(ls.rect.x), static_cast<float>(ls.rect.y));
        }
    }

    // 3. Bottom layer surfaces (desktop widgets/desktop icons)
    for (const auto& ls : backend_->layer_surfaces()) {
        if (!ls.mapped || ls.layer != brocompositor::wl::Layer::Bottom) continue;
        for (const auto& node : backend_->layer_surface_tree(ls.id)) {
            appendSurfaceNode(node, static_cast<float>(ls.rect.x), static_cast<float>(ls.rect.y));
        }
    }

    // 4. xdg-shell client windows & unmanaged X11 surfaces
    auto windows = backend_->windows();
    for (auto winId : windows) {
        if (!backend_->visible(winId)) continue;
        auto optSnap = backend_->query(winId);
        float wx = optSnap ? static_cast<float>(optSnap->frame.x) : 0.0f;
        float wy = optSnap ? static_cast<float>(optSnap->frame.y) : 0.0f;
        for (const auto& node : backend_->window_surfaces(winId)) {
            appendSurfaceNode(node, wx, wy);
        }
    }

    // 5. Top layer surfaces (panels, taskbars, docks)
    for (const auto& ls : backend_->layer_surfaces()) {
        if (!ls.mapped || ls.layer != brocompositor::wl::Layer::Top) continue;
        for (const auto& node : backend_->layer_surface_tree(ls.id)) {
            appendSurfaceNode(node, static_cast<float>(ls.rect.x), static_cast<float>(ls.rect.y));
        }
    }

    // 6. Overlay layer surfaces (notifications, OSD, popups)
    for (const auto& ls : backend_->layer_surfaces()) {
        if (!ls.mapped || ls.layer != brocompositor::wl::Layer::Overlay) continue;
        for (const auto& node : backend_->layer_surface_tree(ls.id)) {
            appendSurfaceNode(node, static_cast<float>(ls.rect.x), static_cast<float>(ls.rect.y));
        }
    }

    return leasedFrames;
#else
    (void)outLayers;
    return {};
#endif
}

void WaylandCompositor::releaseClientLayers(const std::vector<LeasedSurfaceFrame>& frames) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    for (const auto& lf : frames) {
        if (lf.surface) {
            lf.surface->presented_on(brocompositor::kNoMonitor, 0);
            lf.surface->release(lf.frame);
        }
    }
#else
    (void)frames;
#endif
}

bool WaylandCompositor::focusWindow(uint64_t winId) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    return backend_->focus(static_cast<brocompositor::WindowId>(winId));
#else
    (void)winId;
    return false;
#endif
}

bool WaylandCompositor::closeWindow(uint64_t winId) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    return backend_->close(static_cast<brocompositor::WindowId>(winId));
#else
    (void)winId;
    return false;
#endif
}

bool WaylandCompositor::setWindowState(uint64_t winId, bool maximized, bool fullscreen) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    return backend_->set_window_state(static_cast<brocompositor::WindowId>(winId), maximized, fullscreen);
#else
    (void)winId;
    (void)maximized;
    (void)fullscreen;
    return false;
#endif
}

bool WaylandCompositor::setWindowMinimized(uint64_t winId, bool minimized) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    return backend_->set_window_minimized(static_cast<brocompositor::WindowId>(winId), minimized);
#else
    (void)winId;
    (void)minimized;
    return false;
#endif
}

bool WaylandCompositor::placeWindow(uint64_t winId, int x, int y, int w, int h) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    brocompositor::Rect frame{x, y, w, h};
    return backend_->place(static_cast<brocompositor::WindowId>(winId), frame);
#else
    (void)winId;
    (void)x; (void)y; (void)w; (void)h;
    return false;
#endif
}

std::vector<uint64_t> WaylandCompositor::windows() const {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return {};
    auto wins = backend_->windows();
    std::vector<uint64_t> res;
    res.reserve(wins.size());
    for (auto w : wins) res.push_back(static_cast<uint64_t>(w));
    return res;
#else
    return {};
#endif
}

std::optional<brocompositor::WindowSnapshot> WaylandCompositor::queryWindow(uint64_t winId) const {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return std::nullopt;
    return backend_->query(static_cast<brocompositor::WindowId>(winId));
#else
    (void)winId;
    return std::nullopt;
#endif
}

uint32_t WaylandCompositor::addOutput(uint32_t width, uint32_t height) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return 0;
    brocompositor::Size sz{static_cast<int32_t>(width), static_cast<int32_t>(height)};
    return static_cast<uint32_t>(backend_->add_output(sz));
#else
    (void)width; (void)height;
    return 0;
#endif
}

bool WaylandCompositor::configureOutput(uint32_t outputId, float scale, int32_t x, int32_t y) {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    brocompositor::wl::OutputConfig cfg;
    cfg.scale = scale;
    cfg.position = brocompositor::Point{x, y};
    return backend_->configure_output(static_cast<brocompositor::MonitorId>(outputId), cfg);
#else
    (void)outputId; (void)scale; (void)x; (void)y;
    return false;
#endif
}

std::vector<brocompositor::MonitorSnapshot> WaylandCompositor::monitors() const {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return {};
    return backend_->monitors();
#else
    return {};
#endif
}

bool WaylandCompositor::isSessionLocked() const {
#if defined(__linux__) && BRO_WITH_COMPOSITOR
    if (!backend_) return false;
    return backend_->session_lock_state() != brocompositor::wl::LockState::Unlocked;
#else
    return false;
#endif
}

#if defined(__linux__) && BRO_WITH_COMPOSITOR
std::vector<brocompositor::wl::LayerSurfaceInfo> WaylandCompositor::layerSurfaces() const {
    if (!backend_) return {};
    return backend_->layer_surfaces();
}
#endif

} // namespace bro::compositor
