#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"
#include "util/time.h"

#include <brocompositor/api.h>

namespace bro::compositor {

WaylandCompositor::WaylandCompositor() = default;

WaylandCompositor::~WaylandCompositor() {
    shutdown();
}

bool WaylandCompositor::init(const CompositorConfig& config, std::string* error) {
#if BRO_HAVE_WAYLAND_SERVER
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

    wm_ = std::make_shared<brocompositor::WindowManager>();
    socketName_ = backend_->socket_name();
    running_ = true;
    return true;
#else
    if (error) *error = "brocompositor is not available on this platform or build";
    return false;
#endif
}

void WaylandCompositor::shutdown() {
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) {
        return backend_->xwayland_display();
    }
#endif
    return {};
}

void WaylandCompositor::pollEvents() {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return;

    auto events = backend_->events().drain();
    auto q = brocompositor::api::getEventQueue();
    for (auto& ev : events) {
        if (wm_) {
            auto cmd = wm_->handle(ev);
            if (auto* a = std::get_if<brocompositor::WindowAdded>(&ev)) {
                auto focusCmds = wm_->focus(a->window.id);
                cmd.insert(cmd.end(), focusCmds.begin(), focusCmds.end());
            }
            backend_->execute(cmd);
        }
        if (q) {
            q->push(ev);
        }
    }

    auto sevents = backend_->server_events().drain();
    (void)sevents;
#endif
}

void WaylandCompositor::injectKey(uint32_t keycode, bool pressed) {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) {
        uint32_t t = static_cast<uint32_t>(util::currentTimeMs());
        backend_->keyboard_key(t, keycode, pressed, {});
    }
#else
    (void)keycode; (void)pressed;
#endif
}

void WaylandCompositor::injectPointerMotion(double dx, double dy) {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) backend_->inject_pointer_motion(dx, dy);
#else
    (void)dx; (void)dy;
#endif
}

void WaylandCompositor::injectPointerWarp(double x, double y) {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) backend_->inject_pointer_warp(x, y);
#else
    (void)x; (void)y;
#endif
}

void WaylandCompositor::injectPointerButton(uint32_t button, bool pressed) {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) {
        uint32_t t = static_cast<uint32_t>(util::currentTimeMs());
        backend_->pointer_button(t, button, pressed);
        backend_->pointer_frame();
    }
#else
    (void)button; (void)pressed;
#endif
}

void WaylandCompositor::injectPointerAxis(uint32_t orientation, double delta, int32_t discrete) {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) {
        uint32_t t = static_cast<uint32_t>(util::currentTimeMs());
        backend_->pointer_axis(t, orientation, delta, discrete, 0);
        backend_->pointer_frame();
    }
#else
    (void)orientation; (void)delta; (void)discrete;
#endif
}

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::vector<engine::UILayer>& outLayers) {
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;
    if (wm_) {
        auto cmds = wm_->focus(static_cast<brocompositor::WindowId>(winId));
        backend_->execute(cmds);
    }
    return backend_->focus(static_cast<brocompositor::WindowId>(winId));
#else
    (void)winId;
    return false;
#endif
}

#if BRO_HAVE_WAYLAND_SERVER
brocompositor::wl::CursorChanged WaylandCompositor::cursor() const {
    if (!backend_) return {};
    return backend_->cursor();
}

uint64_t WaylandCompositor::windowAt(double x, double y) const {
    if (!backend_) return 0;
    uint64_t f = focusedWindow();
    if (f != 0 && backend_->visible(f)) {
        auto snap = backend_->query(f);
        if (snap && backend_->hit_test(f, x - snap->frame.x, y - snap->frame.y)) {
            return f;
        }
    }
    for (auto w : backend_->windows()) {
        if (w == f || !backend_->visible(w)) continue;
        auto snap = backend_->query(w);
        if (snap && backend_->hit_test(w, x - snap->frame.x, y - snap->frame.y)) {
            return w;
        }
    }
    return 0;
}

bool WaylandCompositor::routePointer(double x, double y, uint32_t time) {
    if (!backend_) return false;

    if (backend_->session_lock_state() != brocompositor::wl::LockState::Unlocked) {
        for (const auto& mon : backend_->monitors()) {
            if (x >= mon.bounds.x && x < mon.bounds.x + mon.bounds.width &&
                y >= mon.bounds.y && y < mon.bounds.y + mon.bounds.height) {
                if (auto hit = backend_->hit_test_lock(mon.id, x - mon.bounds.x, y - mon.bounds.y)) {
                    backend_->pointer_route(hit->surface, hit->sx, hit->sy, time);
                    backend_->pointer_frame();
                    return true;
                }
            }
        }
        backend_->pointer_route(brocompositor::wl::kNoSurface, 0, 0, time);
        backend_->pointer_frame();
        return false;
    }

    for (const auto& u : backend_->unmanaged_surfaces()) {
        if (x >= u.rect.x && x < u.rect.x + u.rect.width &&
            y >= u.rect.y && y < u.rect.y + u.rect.height) {
            backend_->pointer_route(u.surface, x - u.rect.x, y - u.rect.y, time);
            backend_->pointer_frame();
            return true;
        }
    }

    uint64_t hitWin = windowAt(x, y);
    if (hitWin != 0) {
        auto snap = backend_->query(hitWin);
        if (snap) {
            if (auto hit = backend_->hit_test(hitWin, x - snap->frame.x, y - snap->frame.y)) {
                backend_->pointer_route(hit->surface, hit->sx, hit->sy, time);
                backend_->pointer_frame();
                return true;
            }
        }
    }

    backend_->pointer_route(brocompositor::wl::kNoSurface, 0, 0, time);
    backend_->pointer_frame();
    return false;
}
#endif

bool WaylandCompositor::closeWindow(uint64_t winId) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;
    return backend_->close(static_cast<brocompositor::WindowId>(winId));
#else
    (void)winId;
    return false;
#endif
}

bool WaylandCompositor::setWindowState(uint64_t winId, bool maximized, bool fullscreen) {
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;
    return backend_->set_window_minimized(static_cast<brocompositor::WindowId>(winId), minimized);
#else
    (void)winId;
    (void)minimized;
    return false;
#endif
}

bool WaylandCompositor::placeWindow(uint64_t winId, int x, int y, int w, int h) {
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return std::nullopt;
    return backend_->query(static_cast<brocompositor::WindowId>(winId));
#else
    (void)winId;
    return std::nullopt;
#endif
}

uint64_t WaylandCompositor::focusedWindow() const {
#if BRO_HAVE_WAYLAND_SERVER
    return wm_ ? wm_->focused() : 0;
#else
    return 0;
#endif
}

uint32_t WaylandCompositor::addOutput(uint32_t width, uint32_t height) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return 0;
    brocompositor::Size sz{static_cast<int32_t>(width), static_cast<int32_t>(height)};
    return static_cast<uint32_t>(backend_->add_output(sz));
#else
    (void)width; (void)height;
    return 0;
#endif
}

bool WaylandCompositor::configureOutput(uint32_t outputId, float scale, int32_t x, int32_t y) {
#if BRO_HAVE_WAYLAND_SERVER
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
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return {};
    return backend_->monitors();
#else
    return {};
#endif
}

bool WaylandCompositor::isSessionLocked() const {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;
    return backend_->session_lock_state() != brocompositor::wl::LockState::Unlocked;
#else
    return false;
#endif
}

#if BRO_HAVE_WAYLAND_SERVER
std::vector<brocompositor::wl::LayerSurfaceInfo> WaylandCompositor::layerSurfaces() const {
    if (!backend_) return {};
    return backend_->layer_surfaces();
}
#endif

} // namespace bro::compositor
