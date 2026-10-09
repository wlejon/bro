// bro.remote's host side. broremote_api owns the Server and tells this file,
// through HostHooks::serverChanged, when one starts and just before one goes.
// Between the two, once per frame on the engine thread (a frame pump):
//
//   events   tickRemote() delivers attach / detach to the page's listeners.
//   input    The viewers' input (evdev codes, stream pixels) goes to
//            Engine::injectDeviceInput, so it is routed exactly as local
//            input: under DRM through the libinput path (hotkeys, the shell
//            or a client window), elsewhere through the handle* entry points.
//   cursor   The screen's pointer shape and position, for the viewer.
//   frames   Only while Server::wants_frames() (a viewer is attached):
//              DRM       every composited frame, as the KMS scanout buffer it
//                        is: a dmabuf the encoder imports, no copy. The
//                        presenter calls back as soon as each composite's
//                        GPU work is done, before its flip (encoding
//                        starts without waiting for the vblank); the slot is
//                        held (KmsDirectPresenter::holdSlot) until the server
//                        releases the frame, and the presenter waits (briefly,
//                        bounded) before drawing into a held slot. Direct
//                        scanout of a fullscreen client is inhibited, so the
//                        client is composited where the stream sees it.
//              windowed  each present is also read back (the presenter's
//                        capture-presents mode) and submitted as a CPU frame,
//                        at most `fps` a second.
//              headless  the frame is composited on demand (capturePixels),
//                        on the virtual clock, at most `fps` a second.
//              server    no frames (bro-server renders nothing).
//
// With no server the pump is one branch, and no presenter state is touched.
#include "bronze_host/host_remote.h"
#include "bronze_host/host_remote_view.h"

#include "engine/engine.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF && defined(__linux__)
#include "render/kms_direct_presenter.h"
#endif
#include "util/log.h"

#include "embed/embed.h"

#include <broremote/api.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using engine::DisplayMode;

struct RemoteHost {
    engine::Engine* engine = nullptr;
    broremote::Server* server = nullptr;
    broremote::ServerConfig config;
    std::vector<broremote::InputEvent> input;
    bool feeding = false;  // the frame source is set up (wants_frames was true)
    double lastCpuFrameMs = -std::numeric_limits<double>::infinity();
    // Windowed: the presenter's capture-presents setting before hosting.
    bool captureWas = false;
    bool captureSet = false;
    bool loggedNoFrames = false;
#if BRO_WITH_DMABUF && defined(__linux__)
    render::KmsDirectPresenter* kms = nullptr;  // the presenter whose listener is ours
#endif
};
RemoteHost g_host;

// Stream pixels and CSS px, for the pointer the viewer is told about.
float cssToFrame(float css, int cssExtent, int frameExtent) {
    if (cssExtent <= 0 || frameExtent <= 0) return css;
    return css * static_cast<float>(frameExtent) / static_cast<float>(cssExtent);
}

#if BRO_WITH_DMABUF && defined(__linux__)
void onScanout(const render::KmsScanoutFrame& f) {
    broremote::Server* server = g_host.server;
    render::KmsDirectPresenter* kms = g_host.kms;
    if (!server || !kms || !server->wants_frames() || f.planeCount == 0) return;
    broremote::Frame frame;
    frame.width = f.width;
    frame.height = f.height;
    frame.drm_format = f.drmFormat;
    frame.modifier = f.modifier;
    frame.plane_count = f.planeCount;
    for (uint32_t i = 0; i < f.planeCount && i < 4; ++i) {
        frame.planes[i].fd = f.fds[i];
        frame.planes[i].offset = f.offsets[i];
        frame.planes[i].pitch = f.strides[i];
    }
    // presentComposited waited for the slot's GPU work before the flip.
    frame.acquire_fence_fd = -1;
    std::function<void()> release = kms->holdSlot(f.slot);
    server->submit(frame, std::move(release));
}
#endif

// Starts or stops feeding the server frames (a viewer came or the last went).
void setFeeding(bool on) {
    if (on == g_host.feeding) return;
    g_host.feeding = on;
    engine::Engine& e = *g_host.engine;
    render::VulkanPresenter* presenter = e.vulkanPresenter();
    g_host.lastCpuFrameMs = -std::numeric_limits<double>::infinity();
    switch (e.displayMode()) {
        case DisplayMode::Drm: {
#if BRO_WITH_DMABUF && defined(__linux__)
            if (on) {
                render::KmsDirectPresenter* kms = presenter ? presenter->kmsDirectPresenter() : nullptr;
                if (!kms || !kms->isActive()) {
                    LOG_WARN("bro.remote: no KMS presenter; the stream gets no frames");
                    return;
                }
                g_host.kms = kms;
                kms->setDirectScanoutInhibited(true);
                kms->setScanoutListener(&onScanout);
            } else if (g_host.kms) {
                g_host.kms->setScanoutListener(nullptr);
                g_host.kms->setDirectScanoutInhibited(false);
                g_host.kms = nullptr;
            }
#endif
            break;
        }
        case DisplayMode::Windowed:
            if (!presenter || presenter->isHeadless()) break;
            if (on) {
                g_host.captureWas = presenter->capturePresents();
                g_host.captureSet = true;
                presenter->setCapturePresents(true);
            } else if (g_host.captureSet) {
                presenter->setCapturePresents(g_host.captureWas);
                g_host.captureSet = false;
            }
            break;
        case DisplayMode::Headless:
        case DisplayMode::Server:
            break;
    }
}

// Windowed and headless: the composited frame read back to the CPU, at most
// `fps` a second on the engine's activity clock (virtual in headless).
void submitCpuFrame() {
    engine::Engine& e = *g_host.engine;
    const DisplayMode mode = e.displayMode();
    if (mode != DisplayMode::Windowed && mode != DisplayMode::Headless) return;
    const double now = e.activityClockMs();
    const double interval = 1000.0 / static_cast<double>(std::max<uint32_t>(1, g_host.config.fps));
    if (now - g_host.lastCpuFrameMs < interval) return;

    auto pixels = std::make_shared<std::vector<uint8_t>>();
    int w = 0, h = 0;
    if (mode == DisplayMode::Headless) {
        // A composite records the document as laid out; lay it out first.
        if (e.document()) e.flushLayoutForRead(e.document());
        *pixels = e.capturePixels();
        w = e.framePixelWidth();
        h = e.framePixelHeight();
    } else {
        *pixels = e.presentedPixels(0, w, h);
    }
    if (w <= 0 || h <= 0 || pixels->size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        if (!g_host.loggedNoFrames) {
            g_host.loggedNoFrames = true;
            LOG_WARN("bro.remote: no frame to send (nothing presented yet, or no GPU presenter)");
        }
        return;
    }
    g_host.lastCpuFrameMs = now;
    broremote::Frame frame;
    frame.width = static_cast<uint32_t>(w);
    frame.height = static_cast<uint32_t>(h);
    frame.cpu = pixels->data();
    // The buffer lives until the server is done with it.
    g_host.server->submit(frame, [pixels] {});
}

engine::Engine::DeviceInput toDeviceInput(const broremote::InputEvent& in) {
    using Kind = engine::Engine::DeviceInput::Kind;
    engine::Engine::DeviceInput out;
    out.code = in.code;
    out.pressed = in.pressed;
    switch (in.kind) {
        case broremote::InputKind::Key: out.kind = Kind::Key; break;
        case broremote::InputKind::PointerMotion:
            out.kind = Kind::Motion;
            out.x = in.x;
            out.y = in.y;
            break;
        case broremote::InputKind::RelativeMotion:
            out.kind = Kind::RelativeMotion;
            out.x = in.x;
            out.y = in.y;
            break;
        case broremote::InputKind::Button: out.kind = Kind::Button; break;
        case broremote::InputKind::Wheel:
            out.kind = Kind::Wheel;
            out.wheelX = in.wheel_x;
            out.wheelY = in.wheel_y;
            break;
    }
    return out;
}

void pump() {
    broremote::api::tickRemote();
    if (ev::microtasksPending()) ev::drainMicrotasks();
    if (!g_host.server) return;
    engine::Engine& e = *g_host.engine;

    // A handler may stop hosting (bro.remote.stop()) partway through.
    g_host.input.clear();
    g_host.server->drain_input(g_host.input);
    for (const auto& in : g_host.input) {
        e.injectDeviceInput(toDeviceInput(in));
        if (!g_host.server) return;
    }

    broremote::CursorState cursor;
    cursor.shape = e.screenCursorShape();
    cursor.visible = cursor.shape != "none" && (e.displayMode() != DisplayMode::Drm || e.isCursorVisible());
    cursor.locked = e.screenPointerLocked();
    cursor.x = static_cast<int32_t>(
        std::lround(cssToFrame(e.getLastMouseX(), e.viewportWidth(), e.framePixelWidth())));
    cursor.y = static_cast<int32_t>(
        std::lround(cssToFrame(e.getLastMouseY(), e.viewportHeight(), e.framePixelHeight())));
    g_host.server->set_cursor(cursor);

    setFeeding(g_host.server->wants_frames());
    if (g_host.feeding) submitCpuFrame();
}

void serverChanged(broremote::Server* server, const broremote::ServerConfig& config) {
    if (server) {
        g_host.server = server;
        g_host.config = config;
        g_host.loggedNoFrames = false;
        LOG_INFO("bro.remote: hosting on %s", server->socket_path().c_str());
        return;
    }
    // The server is about to go: nothing may reach it after this returns.
    setFeeding(false);
    g_host.server = nullptr;
    LOG_INFO("bro.remote: stopped hosting");
}

}  // namespace

void installRemoteHost(engine::Engine& engine) {
    static bool hooksInstalled = false;
    if (!hooksInstalled) {
        hooksInstalled = true;
        g_host.engine = &engine;
        broremote::api::HostHooks hooks;
        hooks.serverChanged = &serverChanged;
        broremote::api::setHostHooks(std::move(hooks));
        engine.addFramePump(&pump);
        engine.addShutdownHook([] { broremote::api::shutdownRemote(); });
        // The viewer side: <remoteview> and bro.remote.connect()'s sessions.
        installRemoteViewHost(engine);
    }
    broremote::api::installRemote();
}

}  // namespace bro::bronze_host
