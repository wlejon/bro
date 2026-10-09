#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"
#include "util/time.h"

#include <brocompositor/api.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#if BRO_HAVE_WAYLAND_SERVER
#include <sys/mman.h>
#endif

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

    // New windows map on top and focused; focusing raises (the window
    // manager's stacking order is the one the composite and hit tests use).
    brocompositor::WindowManagerConfig wmCfg;
    wmCfg.focus_on_map = true;
    wm_ = std::make_shared<brocompositor::WindowManager>(wmCfg);
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

bool WaylandCompositor::pollEvents() {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;

    bool hadEvents = false;
    auto events = backend_->events().drain();
    if (!events.empty()) hadEvents = true;

    auto q = brocompositor::api::getEventQueue();
    for (auto& ev : events) {
        if (wm_) {
            backend_->execute(wm_->handle(ev));
        }
        if (q) {
            q->push(ev);
        }
    }

    auto sevents = backend_->server_events().drain();
    // A client's commit, an output's frame and the seat's own input change
    // nothing the shell draws: client content is sampled where it is each
    // frame, and a frame that sampled nothing new is not presented. Counting
    // them had the shell re-rastered for every client frame (or frame
    // callback) and every pointer motion.
    for (const auto& sev : sevents) {
        using namespace brocompositor::wl;
        if (std::holds_alternative<SurfaceCommitted>(sev) || std::holds_alternative<OutputFrame>(sev) ||
            std::holds_alternative<OutputPresented>(sev) || std::holds_alternative<PointerMotion>(sev) ||
            std::holds_alternative<PointerButton>(sev) || std::holds_alternative<PointerAxis>(sev) ||
            std::holds_alternative<PointerFrame>(sev) || std::holds_alternative<KeyboardKey>(sev))
            continue;
        hadEvents = true;
        break;
    }

    using Kind = brocompositor::wl::WindowRequestKind;
    for (const auto& sev : sevents) {
        if (auto* pc = std::get_if<brocompositor::wl::PointerConstraintChanged>(&sev)) {
            using K = brocompositor::wl::PointerConstraintKind;
            pointerConstraint_ = pc->kind == K::Locked     ? PointerConstraint::Locked
                                 : pc->kind == K::Confined ? PointerConstraint::Confined
                                                           : PointerConstraint::None;
            continue;
        }
        auto* req = std::get_if<brocompositor::wl::WindowRequest>(&sev);
        if (!req) continue;
        if (req->kind == Kind::Move) {
            startInteractiveMove(req->window, lastPointerX_, lastPointerY_, true);
        } else if (req->kind == Kind::Resize) {
            startInteractiveResize(req->window, lastPointerX_, lastPointerY_, req->edges, true);
        } else if (req->kind == Kind::Close) {
            closeWindow(req->window);
        } else if (req->kind == Kind::Activate) {
            // xdg-activation (a client raising itself with a token wlroots
            // validated: a single-instance app handed a second launch) or a
            // taskbar's foreign-toplevel activate. A minimized window comes
            // back first.
            if (wm_) {
                const auto w = wm_->window(req->window);
                if (w && w->snapshot.minimized) backend_->execute(wm_->restore(req->window));
            }
            focusWindow(req->window);
        } else if (wm_ && wm_->window(req->window)) {
            // State requests (the client's own buttons, a taskbar) go through
            // the window manager, so maximize fills the work area the shell
            // left free and restore returns the remembered frame.
            std::vector<brocompositor::Command> cmds;
            switch (req->kind) {
                case Kind::Maximize: cmds = wm_->maximize(req->window); break;
                case Kind::Fullscreen: cmds = wm_->fullscreen(req->window); break;
                case Kind::Minimize: cmds = wm_->minimize(req->window); break;
                case Kind::Unmaximize:
                case Kind::Unfullscreen:
                case Kind::Unminimize: cmds = wm_->restore(req->window); break;
                default: break;
            }
            backend_->execute(cmds);
        } else if (req->kind == Kind::Maximize) {
            setWindowState(req->window, true, false);
        } else if (req->kind == Kind::Unmaximize || req->kind == Kind::Unfullscreen) {
            setWindowState(req->window, false, false);
        } else if (req->kind == Kind::Fullscreen) {
            setWindowState(req->window, false, true);
        } else if (req->kind == Kind::Minimize) {
            setWindowMinimized(req->window, true);
        } else if (req->kind == Kind::Unminimize) {
            setWindowMinimized(req->window, false);
        }
    }

    return hadEvents;
#else
    return false;
#endif
}

void WaylandCompositor::injectKey(uint32_t keycode, bool pressed) {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_) {
        uint32_t t = static_cast<uint32_t>(util::currentTimeMs());
        // The modifiers that follow the key come from the seat's keymap
        // (brocompositor tracks them): with {} here clients never saw Shift,
        // and typed "4" for "$".
        backend_->keyboard_key(t, keycode, pressed);
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

void WaylandCompositor::injectPointerWarp(double x, double y, double dx, double dy) {
#if BRO_HAVE_WAYLAND_SERVER
    lastPointerX_ = x;
    lastPointerY_ = y;
    if (backend_) backend_->inject_pointer_warp(x, y, dx, dy);
#else
    (void)x; (void)y; (void)dx; (void)dy;
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

#if BRO_HAVE_WAYLAND_SERVER
namespace {

// Leases a surface's newest frame and adds it as a layer at (baseX, baseY) +
// the node's offset. Only dmabuf buffers are composited.
void appendSurfaceNode(brocompositor::wl::ServerBackend& backend, const brocompositor::wl::SurfaceNode& node,
                       float baseX, float baseY, std::vector<engine::UILayer>& outLayers,
                       std::vector<LeasedSurfaceFrame>& leasedFrames, float scaleX = 1.0f, float scaleY = 1.0f) {
    auto surface = backend.surface(node.surface);
    if (!surface) return;
    auto frameOpt = surface->acquire();
    if (!frameOpt) return;
    auto imgOpt = surface->image(frameOpt->image_id);
    if (!imgOpt || imgOpt->type != brocompositor::ImageHandleType::DmaBuf) {
        surface->release(*frameOpt);
        return;
    }
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
    layer.quad.x = baseX + static_cast<float>(node.offset.x) * scaleX;
    layer.quad.y = baseY + static_cast<float>(node.offset.y) * scaleY;
    // Logical size (a scale-2 buffer covers half its pixels).
    const bool logical = node.size.width > 0 && node.size.height > 0;
    layer.quad.w = static_cast<float>(logical ? node.size.width : int32_t(imgOpt->width)) * scaleX;
    layer.quad.h = static_cast<float>(logical ? node.size.height : int32_t(imgOpt->height)) * scaleY;
    layer.quad.clipW = -1.0f;
    layer.content = dmabufSrc;
    outLayers.push_back(layer);

    LeasedSurfaceFrame leased;
    leased.surfaceId = static_cast<uint32_t>(node.surface);
    leased.surface = surface;
    leased.frame = *frameOpt;
    leasedFrames.push_back(std::move(leased));
}

void appendLayerShell(brocompositor::wl::ServerBackend& backend, brocompositor::wl::Layer which,
                      std::vector<engine::UILayer>& out, std::vector<LeasedSurfaceFrame>& leased) {
    for (const auto& ls : backend.layer_surfaces()) {
        if (!ls.mapped || ls.layer != which) continue;
        for (const auto& node : backend.layer_surface_tree(ls.id))
            appendSurfaceNode(backend, node, static_cast<float>(ls.rect.x), static_cast<float>(ls.rect.y), out, leased);
    }
}

}  // namespace
#endif

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::vector<engine::UILayer>& outLayers) {
#if BRO_HAVE_WAYLAND_SERVER
    std::vector<render::ClientWindowRef> refs;
    for (const auto& w : stack()) refs.push_back(render::ClientWindowRef{w.id, false, 0.0f, 0.0f});
    return acquireClientLayers(refs, render::kClientLayersBelow | render::kClientLayersAbove, outLayers);
#else
    (void)outLayers;
    return {};
#endif
}

std::vector<LeasedSurfaceFrame> WaylandCompositor::acquireClientLayers(std::span<const render::ClientWindowRef> windows,
                                                                       uint32_t parts,
                                                                       std::vector<engine::UILayer>& outLayers) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return {};
    auto& b = *backend_;
    std::vector<LeasedSurfaceFrame> leased;
    using Layer = brocompositor::wl::Layer;

    // Locked: the lock surfaces alone, above everything.
    if (b.session_lock_state() != brocompositor::wl::LockState::Unlocked) {
        if (!(parts & render::kClientLayersAbove)) return leased;
        for (const auto& mon : b.monitors())
            for (const auto& node : b.lock_surface_tree(mon.id))
                appendSurfaceNode(b, node, static_cast<float>(mon.bounds.x), static_cast<float>(mon.bounds.y),
                                  outLayers, leased);
        return leased;
    }

    if (parts & render::kClientLayersBelow) {
        appendLayerShell(b, Layer::Background, outLayers, leased);
        appendLayerShell(b, Layer::Bottom, outLayers, leased);
    }
    for (const auto& ref : windows) {
        if (!b.visible(ref.windowId)) continue;
        float wx = ref.x, wy = ref.y;
        float sx = 1.0f, sy = 1.0f;
        if (!ref.pinned) {
            auto snap = b.query(ref.windowId);
            if (!snap) continue;
            wx = static_cast<float>(snap->frame.x);
            wy = static_cast<float>(snap->frame.y);
        } else if (ref.w > 0.0f && ref.h > 0.0f) {
            // Shown at its frame's size: scaled while that differs from the
            // size the client drew at (a window gliding between states).
            if (auto snap = b.query(ref.windowId); snap && snap->frame.width > 0 && snap->frame.height > 0) {
                const float fw = static_cast<float>(snap->frame.width);
                const float fh = static_cast<float>(snap->frame.height);
                if (std::abs(ref.w - fw) > 0.5f || std::abs(ref.h - fh) > 0.5f) {
                    sx = ref.w / fw;
                    sy = ref.h / fh;
                }
            }
        }
        for (const auto& node : b.window_surfaces(ref.windowId))
            appendSurfaceNode(b, node, wx, wy, outLayers, leased, sx, sy);
    }
    if (parts & render::kClientLayersAbove) {
        // Override-redirect X11 surfaces (menus, tooltips) above every window.
        for (const auto& u : b.unmanaged_surfaces()) {
            brocompositor::wl::SurfaceNode node;
            node.surface = u.surface;
            node.size = brocompositor::Size{u.rect.width, u.rect.height};
            appendSurfaceNode(b, node, static_cast<float>(u.rect.x), static_cast<float>(u.rect.y), outLayers, leased);
        }
        appendLayerShell(b, Layer::Top, outLayers, leased);
        appendLayerShell(b, Layer::Overlay, outLayers, leased);
    }
    return leased;
#else
    (void)windows;
    (void)parts;
    (void)outLayers;
    return {};
#endif
}

bool WaylandCompositor::acquireDragIcon(float px, float py, std::vector<engine::UILayer>& outLayers,
                                        std::vector<LeasedSurfaceFrame>& leased, DragIconPixels& pixels) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return false;
    const auto node = backend_->drag_icon();
    if (!node) {
        dragIconSurface_ = 0;
        dragIconPixels_.clear();
        return false;
    }
    auto surface = backend_->surface(node->surface);
    if (!surface) return false;
    const float x = px + static_cast<float>(node->offset.x), y = py + static_cast<float>(node->offset.y);
    auto frame = surface->acquire();
    if (!frame) return false;
    auto img = surface->image(frame->image_id);
    if (img && img->type == brocompositor::ImageHandleType::DmaBuf) {
        surface->release(*frame);
        const size_t before = outLayers.size();
        appendSurfaceNode(*backend_, brocompositor::wl::SurfaceNode{node->surface, {}, node->size, false}, x, y,
                          outLayers, leased);
        return outLayers.size() > before;
    }
    // A shm icon: its pixels, copied once per frame of it.
    constexpr uint32_t kArgb8888 = 0x34325241, kXrgb8888 = 0x34325258;  // 'AR24', 'XR24'
    const bool shm = img && img->type == brocompositor::ImageHandleType::ShmFd && !img->planes.empty() &&
                     (img->drm_format == kArgb8888 || img->drm_format == kXrgb8888);
    if (shm && (dragIconSurface_ != node->surface || dragIconSequence_ != frame->sequence)) {
        const auto& plane = img->planes[0];
        const size_t rowBytes = static_cast<size_t>(img->width) * 4;
        const size_t len = static_cast<size_t>(plane.offset) + static_cast<size_t>(plane.stride) * img->height;
        const int fd = brocompositor::wl::fd_of(plane.handle);
        void* map = fd >= 0 && plane.stride >= rowBytes ? ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        if (map != MAP_FAILED) {
            dragIconPixels_.resize(rowBytes * img->height);
            const auto* src = static_cast<const uint8_t*>(map) + plane.offset;
            for (uint32_t row = 0; row < img->height; ++row)
                std::memcpy(dragIconPixels_.data() + row * rowBytes, src + static_cast<size_t>(row) * plane.stride, rowBytes);
            ::munmap(map, len);
            if (img->drm_format == kXrgb8888)
                for (size_t i = 3; i < dragIconPixels_.size(); i += 4) dragIconPixels_[i] = 255;
            dragIconSurface_ = node->surface;
            dragIconSequence_ = frame->sequence;
            dragIconW_ = static_cast<int>(img->width);
            dragIconH_ = static_cast<int>(img->height);
        }
    }
    surface->release(*frame);
    // The frame callbacks the icon waits on: sent as shown now.
    surface->presented_on(brocompositor::kNoMonitor, 0);
    if (!shm || dragIconSurface_ != node->surface || dragIconPixels_.empty()) return false;
    pixels.x = x;
    pixels.y = y;
    pixels.w = static_cast<float>(node->size.width > 0 ? node->size.width : dragIconW_);
    pixels.h = static_cast<float>(node->size.height > 0 ? node->size.height : dragIconH_);
    pixels.pixelW = dragIconW_;
    pixels.pixelH = dragIconH_;
    pixels.bgra = &dragIconPixels_;
    pixels.key = (static_cast<uint64_t>(node->surface) << 40) ^ dragIconSequence_;
    return true;
#else
    (void)px;
    (void)py;
    (void)outLayers;
    (void)leased;
    (void)pixels;
    return false;
#endif
}

std::vector<ClientWindowInfo> WaylandCompositor::stack() const {
    std::vector<ClientWindowInfo> out;
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_) return out;
    std::vector<brocompositor::WindowId> order;
    if (wm_) order = wm_->stacking();
    // A window the manager has not taken in yet (its event is still queued)
    // is the newest: on top.
    for (auto id : backend_->windows())
        if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
    const brocompositor::WindowId focused = wm_ ? wm_->focused() : brocompositor::kNoWindow;
    for (auto id : order) {
        if (!backend_->visible(id)) continue;
        auto snap = backend_->query(id);
        if (!snap || snap->minimized) continue;
        ClientWindowInfo w;
        w.id = id;
        w.frame = snap->frame;
        w.focused = id == focused;
        w.maximized = snap->maximized;
        w.fullscreen = snap->fullscreen;
        w.snap = "none";
        if (wm_) {
            w.insets = wm_->decoration_insets(id);
            w.framed = wm_->framed(id);
            if (auto v = wm_->window(id)) w.snap = brocompositor::to_string(v->snap);
        }
        out.push_back(std::move(w));
    }
#endif
    return out;
}

void WaylandCompositor::releaseClientLayers(const std::vector<LeasedSurfaceFrame>& frames,
                                            const FramePresentation* shown) {
    notifyClientLayersShown(frames, shown);
    returnClientLayers(frames);
}

void WaylandCompositor::returnClientLayers(const std::vector<LeasedSurfaceFrame>& frames) {
#if BRO_HAVE_WAYLAND_SERVER
    for (const auto& lf : frames)
        if (lf.surface) lf.surface->release(lf.frame);
#else
    (void)frames;
#endif
}

void WaylandCompositor::notifyClientLayersShown(const std::vector<LeasedSurfaceFrame>& frames,
                                                const FramePresentation* shown) {
#if BRO_HAVE_WAYLAND_SERVER
    // With the flip that showed them, the clients hear when their frames
    // turned to light (wp_presentation feedback, on the output it happened
    // on); without one, only their frame callbacks, stamped now.
    brocompositor::wl::PresentationTime t;
    if (shown && backend_) {
        const auto mons = backend_->monitors();
        if (!mons.empty()) t.output = mons.front().id;
        t.timestamp_ns = shown->timestampNs;
        t.sequence = shown->sequence;
        t.refresh_ns = shown->refreshNs;
        t.flags = 0x1 | 0x2 | 0x4;  // vsync, hw clock, hw completion: a KMS flip event
    }
    for (const auto& lf : frames)
        if (lf.surface) lf.surface->presented_frame(lf.frame, t);
#else
    (void)frames;
    (void)shown;
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
    if (isDragActive()) {
        brocompositor::wl::CursorChanged c;
        const auto d = wm_->drag();
        using namespace brocompositor::resize_edge;
        if (d->action == brocompositor::PressAction::Resize) {
            const uint32_t e = d->edges;
            if ((e & Top) && (e & Left)) c.shape = "nw-resize";
            else if ((e & Top) && (e & Right)) c.shape = "ne-resize";
            else if ((e & Bottom) && (e & Left)) c.shape = "sw-resize";
            else if ((e & Bottom) && (e & Right)) c.shape = "se-resize";
            else if (e & (Top | Bottom)) c.shape = "ns-resize";
            else c.shape = "ew-resize";
        } else {
            c.shape = "move";
        }
        return c;
    }
    if (!backend_) return {};
    return backend_->cursor();
}

uint64_t WaylandCompositor::windowAt(double x, double y) const {
    if (!backend_) return 0;
    auto order = stack();
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (windowSurfaceAt(it->id, x, y)) return it->id;
    return 0;
}

bool WaylandCompositor::windowSurfaceAt(uint64_t winId, double x, double y) const {
    if (!backend_) return false;
    auto snap = backend_->query(static_cast<brocompositor::WindowId>(winId));
    return snap && backend_->hit_test(static_cast<brocompositor::WindowId>(winId), x - snap->frame.x,
                                      y - snap->frame.y).has_value();
}

bool WaylandCompositor::unmanagedAt(double x, double y) const {
    if (!backend_ || backend_->session_lock_state() != brocompositor::wl::LockState::Unlocked) return false;
    for (const auto& u : backend_->unmanaged_surfaces())
        if (x >= u.rect.x && x < u.rect.x + u.rect.width && y >= u.rect.y && y < u.rect.y + u.rect.height)
            return true;
    return false;
}

brocompositor::PressDecision WaylandCompositor::classifyPress(uint64_t winId, double x, double y,
                                                             uint32_t modifiers,
                                                             brocompositor::PressButton button) const {
    if (!wm_) return {};
    brocompositor::Point p{static_cast<int32_t>(std::floor(x)), static_cast<int32_t>(std::floor(y))};
    return wm_->classify_press(static_cast<brocompositor::WindowId>(winId), p, modifiers, button);
}

bool WaylandCompositor::routePointer(double x, double y, uint32_t time) {
    if (x >= 0.0 && y >= 0.0) {
        lastPointerX_ = x;
        lastPointerY_ = y;
    }
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
    // Through the window manager when it has the window: maximize fills the
    // work area less the shell's frame, restore returns the remembered frame.
    if (wm_ && wm_->window(static_cast<brocompositor::WindowId>(winId))) {
        const auto id = static_cast<brocompositor::WindowId>(winId);
        backend_->execute(fullscreen ? wm_->fullscreen(id) : maximized ? wm_->maximize(id) : wm_->restore(id));
        return true;
    }
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
    if (wm_ && wm_->window(static_cast<brocompositor::WindowId>(winId))) {
        const auto id = static_cast<brocompositor::WindowId>(winId);
        backend_->execute(minimized ? wm_->minimize(id) : wm_->restore(id));
        return true;
    }
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

// Interactive move / resize: the window manager's drag (begin_move /
// begin_resize / drag_to / end_drag), whose commands go to the backend. It
// raises and focuses the window, waits for a non-immediate drag to pass the
// threshold, restores a maximized or snapped window dragged by its title bar,
// and snaps at monitor edges.
bool WaylandCompositor::startInteractiveMove(uint64_t winId, double startX, double startY, bool immediate) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_ || !wm_ || winId == 0) return false;
    lastPointerX_ = startX;
    lastPointerY_ = startY;
    brocompositor::Point p{static_cast<int32_t>(std::floor(startX)), static_cast<int32_t>(std::floor(startY))};
    backend_->execute(wm_->begin_move(static_cast<brocompositor::WindowId>(winId), p, immediate));
    auto d = wm_->drag();
    return d && d->window == winId;
#else
    (void)winId; (void)startX; (void)startY; (void)immediate;
    return false;
#endif
}

bool WaylandCompositor::startInteractiveResize(uint64_t winId, double startX, double startY, uint32_t edges, bool immediate) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_ || !wm_ || winId == 0) return false;
    lastPointerX_ = startX;
    lastPointerY_ = startY;
    brocompositor::Point p{static_cast<int32_t>(std::floor(startX)), static_cast<int32_t>(std::floor(startY))};
    backend_->execute(wm_->begin_resize(static_cast<brocompositor::WindowId>(winId), p, edges, immediate));
    auto d = wm_->drag();
    return d && d->window == winId;
#else
    (void)winId; (void)startX; (void)startY; (void)edges; (void)immediate;
    return false;
#endif
}

bool WaylandCompositor::updateInteractiveDrag(double curX, double curY) {
#if BRO_HAVE_WAYLAND_SERVER
    if (!backend_ || !wm_ || !wm_->drag()) return false;
    lastPointerX_ = curX;
    lastPointerY_ = curY;
    brocompositor::Point p{static_cast<int32_t>(std::floor(curX)), static_cast<int32_t>(std::floor(curY))};
    auto cmds = wm_->drag_to(p);
    backend_->execute(cmds);
    return !cmds.empty();
#else
    (void)curX; (void)curY;
    return false;
#endif
}

void WaylandCompositor::endInteractiveDrag() {
#if BRO_HAVE_WAYLAND_SERVER
    if (backend_ && wm_ && wm_->drag()) backend_->execute(wm_->end_drag());
#endif
}

bool WaylandCompositor::isDraggingWindow() const {
#if BRO_HAVE_WAYLAND_SERVER
    return wm_ && wm_->drag().has_value();
#else
    return false;
#endif
}

bool WaylandCompositor::isDragActive() const {
#if BRO_HAVE_WAYLAND_SERVER
    if (!wm_) return false;
    auto d = wm_->drag();
    return d && d->active;
#else
    return false;
#endif
}

uint64_t WaylandCompositor::draggedWindow() const {
#if BRO_HAVE_WAYLAND_SERVER
    if (!wm_) return 0;
    auto d = wm_->drag();
    return d ? d->window : 0;
#else
    return 0;
#endif
}

} // namespace bro::compositor
