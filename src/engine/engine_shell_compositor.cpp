// The Wayland compositor a shell host runs: bro as the display server (DRM),
// or a headless engine standing in for one (BRO_HEADLESS_COMPOSITOR=1, for
// tests: the same compositor, window manager, frames and input router, no
// seat and no KMS). Starting it, polling it each frame, and keeping the
// shell's window frames (window_frames.h) on the windows it reports.
#include "engine/engine.h"
#include "engine/engine_drm.h"
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
#include <brocompositor/api.h>
#endif
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
#include "platform/drm_input.h"
#endif
#include "util/log.h"

#include <cmath>
#include <cstdlib>

namespace bro::engine {

bool Engine::startShellCompositor(uint32_t width, uint32_t height, const std::string& socketName, bool xwayland) {
#if BRO_WITH_COMPOSITOR
    if (!drmCtx_) drmCtx_ = std::make_unique<DrmPlatformContext>();
    compositor::CompositorConfig cfg;
    cfg.headless = true;
    cfg.drm = false;
    cfg.width = width;
    cfg.height = height;
    cfg.xwayland = xwayland;
    cfg.socketName = socketName;

    drmCtx_->compositor = std::make_unique<compositor::WaylandCompositor>();
    std::string err;
    if (!drmCtx_->compositor->init(cfg, &err)) {
        LOG_WARN("Engine: WaylandCompositor init failed: %s", err.c_str());
        drmCtx_->compositor.reset();
        return false;
    }
    auto* comp = drmCtx_->compositor.get();
    LOG_INFO("Engine: WaylandCompositor started on socket %s", comp->socketName().c_str());
#if defined(_WIN32)
    _putenv_s("WAYLAND_DISPLAY", comp->socketName().c_str());
    if (!comp->xwaylandDisplay().empty()) _putenv_s("DISPLAY", comp->xwaylandDisplay().c_str());
#else
    ::setenv("WAYLAND_DISPLAY", comp->socketName().c_str(), 1);
    if (!comp->xwaylandDisplay().empty()) ::setenv("DISPLAY", comp->xwaylandDisplay().c_str(), 1);
#endif
#if BRO_HAVE_WAYLAND_SERVER
    if (comp->windowManager()) {
        brocompositor::api::setWindowManager(comp->windowManagerShared());
        brocompositor::api::setCommandSink([comp](const std::vector<brocompositor::Command>& cmds) -> size_t {
            if (comp && comp->backend()) return comp->backend()->execute(cmds);
            return cmds.size();
        });
        // pollEvents feeds the window manager; the API only reports.
        brocompositor::api::setHostFeedsEvents(true);
        brocompositor::api::setPointerSource([this](int32_t& x, int32_t& y) {
            x = static_cast<int32_t>(std::floor(lastMouseX_));
            y = static_cast<int32_t>(std::floor(lastMouseY_));
            return true;
        });
    }
#endif
    return true;
#else
    (void)width;
    (void)height;
    (void)socketName;
    (void)xwayland;
    return false;
#endif
}

std::string Engine::shellCompositorSocket() const {
#if BRO_WITH_COMPOSITOR
    if (drmCtx_ && drmCtx_->compositor && drmCtx_->compositor->isRunning())
        return drmCtx_->compositor->socketName();
#endif
    return {};
}

// The compositor's client events, then the frames brought up to date with
// the stack they left. True when anything happened.
bool Engine::pollShellCompositor() {
#if BRO_WITH_COMPOSITOR
    if (!drmCtx_ || !drmCtx_->compositor) return false;
    const bool events = drmCtx_->compositor->pollEvents();
    syncShellWindowFrames();
    return events;
#else
    return false;
#endif
}

void Engine::syncShellWindowFrames() {
#if BRO_WITH_COMPOSITOR
    if (!drmCtx_ || !drmCtx_->compositor) return;
    std::vector<FrameWindow> stack;
    if (isShellApp()) {
        for (const auto& w : drmCtx_->compositor->stack()) {
            FrameWindow f;
            f.id = w.id;
            f.x = w.frame.x;
            f.y = w.frame.y;
            f.width = w.frame.width;
            f.height = w.frame.height;
            if (!w.fullscreen) {
                f.insetLeft = w.insets.left;
                f.insetTop = w.insets.top;
                f.insetRight = w.insets.right;
                f.insetBottom = w.insets.bottom;
                f.framed = w.framed;
            }
            f.focused = w.focused;
            f.maximized = w.maximized;
            f.snap = w.snap;
            stack.push_back(std::move(f));
        }
    }
    // A window shown, hidden or restacked changes the runs a paint pass
    // records: record again (a frame moving dirties the document anyway).
    const auto& was = drmCtx_->frames.stack();
    bool restacked = was.size() != stack.size();
    for (size_t i = 0; !restacked && i < stack.size(); ++i)
        restacked = was[i].id != stack[i].id || was[i].framed != stack[i].framed;
    drmCtx_->frames.sync(isShellApp() ? document_.get() : nullptr, std::move(stack));
    if (restacked) markAppBaseDirty();
#endif
}

bool Engine::injectHostPointer(const std::string& type, float x, float y, int button, float wheelDy) {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    using EvType = platform::DrmInputEvent::Type;
    platform::DrmInputEvent ev;
    if (type == "move") ev.type = EvType::MouseMove;
    else if (type == "down") ev.type = EvType::MouseDown;
    else if (type == "up") ev.type = EvType::MouseUp;
    else if (type == "wheel") ev.type = EvType::MouseWheel;
    else return false;
    ev.x = x;
    ev.y = y;
    ev.dx = x - lastMouseX_;
    ev.dy = y - lastMouseY_;
    ev.button = button > 0 ? button : 1;
    ev.wheelDy = wheelDy;
    // Seen by the shell: whether routing left it for the document.
    noteUserActivity();
    lastMouseX_ = x;
    lastMouseY_ = y;
    cursorVisible_ = true;
    uiDirty_ = true;
    if (routeDrmPointer(ev)) {
        pollShellCompositor();
        return false;
    }
    deliverDrmInputToShell(ev);
    pollShellCompositor();
    return true;
#else
    if (type == "move") handleMouseMove(x, y, x - lastMouseX_, y - lastMouseY_);
    else if (type == "down") handleMouseDown(x, y, button > 0 ? button : 1);
    else if (type == "up") handleMouseUp(x, y, button > 0 ? button : 1);
    else if (type == "wheel") handleWheel(x, y, 0.0f, wheelDy);
    else return false;
    return true;
#endif
}

}  // namespace bro::engine
