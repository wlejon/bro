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
//   text     The text lane's requests (protocol 1.6) go to the engine's
//            agent control dispatcher (ControlServer::post, from the
//            server's I/O thread) and their replies back to the session.
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
//            Each frame carries its damage, what changed since the frame
//            before it (render/frame_damage.h): under DRM the presenter's
//            GPU comparison of the two scanout buffers, elsewhere a CPU
//            comparison of the two read-back frames. The encoder codes a
//            small damage finer, so a line of text rewritten over and over
//            lands sharp instead of as soft as a big change.
//
// With no server the pump is one branch, and no presenter state is touched.
#include "bronze_host/host_remote.h"
#include "bronze_host/host_remote_view.h"

#include "engine/control.h"
#include "engine/engine.h"
#include "render/frame_damage.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF && defined(__linux__)
#include "render/kms_direct_presenter.h"
#endif
#include "util/log.h"

#include "embed/embed.h"

#include <broremote/api.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>
#endif
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;
using engine::DisplayMode;

struct RemoteHostState {
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
    // A composited frame went unsubmitted, so the next one's damage (against
    // the frame before it) is not what changed since the last frame sent.
    bool damageGap = true;
    // Windowed and headless: the last CPU frame submitted, which the next is
    // diffed against for its damage (render/frame_damage.h).
    std::shared_ptr<std::vector<uint8_t>> lastCpuPixels;
    int lastCpuW = 0, lastCpuH = 0;
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTextures[2];
    uint32_t d3dWidth = 0;
    uint32_t d3dHeight = 0;
    uint32_t d3dSlot = 0;
    bool d3dSlotValid[2] = {false, false};
    uint32_t d3dPrevBandY0 = 0;
    uint32_t d3dPrevBandY1 = 0;
    bool d3dDeviceFailed = false;
#endif
#if BRO_WITH_DMABUF && defined(__linux__)
    render::KmsDirectPresenter* kms = nullptr;  // the presenter whose listener is ours
#endif
};
using RemoteHost = RemoteHostState;
RemoteHostState g_host;

// Stream pixels and CSS px, for the pointer the viewer is told about.
float cssToFrame(float css, int cssExtent, int frameExtent) {
    if (cssExtent <= 0 || frameExtent <= 0) return css;
    return css * static_cast<float>(frameExtent) / static_cast<float>(cssExtent);
}

// The frame's damage, from the compositor's comparison of the frame with the
// one before it: brovideo codes a small damage finer, so a line rewritten
// lands sharp at once rather than as soft as the encoder leaves a big change.
void setDamage(broremote::Frame& frame, bool known, const std::vector<render::DamageRect>& damage) {
    frame.damage_known = known && !g_host.damageGap;
    g_host.damageGap = false;
    if (!frame.damage_known) return;
    frame.damage.reserve(damage.size());
    for (const render::DamageRect& r : damage) frame.damage.push_back({r.x, r.y, r.width, r.height});
}

#if BRO_WITH_DMABUF && defined(__linux__)
void onScanout(const render::KmsScanoutFrame& f) {
    broremote::Server* server = g_host.server;
    render::KmsDirectPresenter* kms = g_host.kms;
    if (!server || !kms || !server->wants_frames() || f.planeCount == 0) {
        g_host.damageGap = true;
        return;
    }
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
    setDamage(frame, f.damageKnown, f.damage);
    std::function<void()> release = kms->holdSlot(f.slot);
    server->submit(frame, std::move(release));
}
#endif

// Starts or stops feeding the server frames (a viewer came or the last went).
void setFeeding(bool on) {
    if (on == g_host.feeding) return;
    g_host.feeding = on;
    g_host.damageGap = true;
    g_host.lastCpuPixels.reset();
#if defined(_WIN32)
    g_host.d3dSlotValid[0] = g_host.d3dSlotValid[1] = false;
#endif
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
                // Only a present is read back, and a still page has already
                // presented its last frame (usually before the viewer
                // came): present it again, or the viewer sees nothing until
                // the page next changes.
                e.requestPresent();
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
#if defined(_WIN32)
bool wantsD3D11Frame() {
    if (!g_host.server || g_host.d3dDeviceFailed) return false;
    const char* env = std::getenv("BRO_REMOTE_D3D11");
    if (env && env[0] != '\0' && std::strcmp(env, "0") == 0) return false;
    if (auto st = g_host.server->stream()) {
        const auto* cap = brovideo::best(st->codec, brovideo::Direction::Encode);
        return cap && cap->hardware && cap->backend == brovideo::Backend::MediaFoundation;
    }
    for (broremote::Codec c : g_host.config.codecs) {
        if (const auto* cap = brovideo::best(c, brovideo::Direction::Encode)) {
            return cap->hardware && cap->backend == brovideo::Backend::MediaFoundation;
        }
    }
    return false;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> uploadD3D11Frame(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                         const broremote::Frame& frame) {
    if (!wantsD3D11Frame()) return nullptr;
    if (!g_host.d3dDevice || !g_host.d3dContext) {
        static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, kLevels,
                                       static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION, &g_host.d3dDevice,
                                       nullptr, &g_host.d3dContext);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   kLevels, static_cast<UINT>(std::size(kLevels)), D3D11_SDK_VERSION,
                                   &g_host.d3dDevice, nullptr, &g_host.d3dContext);
        }
        if (FAILED(hr) || !g_host.d3dDevice || !g_host.d3dContext) {
            g_host.d3dDeviceFailed = true;
            g_host.d3dDevice.Reset();
            g_host.d3dContext.Reset();
            return nullptr;
        }
        Microsoft::WRL::ComPtr<ID3D11Multithread> mt;
        if (SUCCEEDED(g_host.d3dDevice.As(&mt)) && mt) mt->SetMultithreadProtected(TRUE);
    }
    if (g_host.d3dWidth != w || g_host.d3dHeight != h || !g_host.d3dTextures[0] || !g_host.d3dTextures[1]) {
        g_host.d3dTextures[0].Reset();
        g_host.d3dTextures[1].Reset();
        g_host.d3dSlotValid[0] = g_host.d3dSlotValid[1] = false;
        g_host.d3dSlot = 0;
        g_host.d3dPrevBandY0 = 0;
        g_host.d3dPrevBandY1 = h;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; ++i) {
            if (FAILED(g_host.d3dDevice->CreateTexture2D(&td, nullptr, &g_host.d3dTextures[i]))) {
                g_host.d3dTextures[0].Reset();
                g_host.d3dTextures[1].Reset();
                return nullptr;
            }
        }
        g_host.d3dWidth = w;
        g_host.d3dHeight = h;
    }
    const uint32_t slot = g_host.d3dSlot & 1u;
    g_host.d3dSlot = slot ^ 1u;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex = g_host.d3dTextures[slot];
    const uint32_t rowPitch = w * 4;
    if (frame.damage_known) {
        uint32_t curY0 = h, curY1 = 0;
        for (const auto& r : frame.damage) {
            curY0 = std::min(curY0, r.y);
            curY1 = std::max(curY1, std::min(h, r.y + r.height));
        }
        if (g_host.d3dSlotValid[slot]) {
            const uint32_t y0 = std::min(curY0, g_host.d3dPrevBandY0);
            const uint32_t y1 = std::max(curY1, g_host.d3dPrevBandY1);
            if (y1 > y0) {
                D3D11_BOX box{0, y0, 0, w, y1, 1};
                const uint8_t* src = rgba + static_cast<size_t>(y0) * rowPitch;
                g_host.d3dContext->UpdateSubresource(tex.Get(), 0, &box, src, rowPitch, 0);
            }
        } else {
            g_host.d3dContext->UpdateSubresource(tex.Get(), 0, nullptr, rgba, rowPitch, 0);
            g_host.d3dSlotValid[slot] = true;
        }
        g_host.d3dPrevBandY0 = curY0;
        g_host.d3dPrevBandY1 = curY1;
    } else {
        g_host.d3dContext->UpdateSubresource(tex.Get(), 0, nullptr, rgba, rowPitch, 0);
        g_host.d3dSlotValid[slot] = true;
        g_host.d3dPrevBandY0 = 0;
        g_host.d3dPrevBandY1 = h;
    }
    return tex;
}
#endif

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
    const auto& last = g_host.lastCpuPixels;
    if (last && g_host.lastCpuW == w && g_host.lastCpuH == h) {
        std::vector<render::DamageRect> damage;
        render::diffFrames(last->data(), pixels->data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                           static_cast<size_t>(w) * 4, damage);
        setDamage(frame, true, damage);
    } else {
        setDamage(frame, false, {});
    }
    // The server only reads it, so the next frame can be diffed against it.
    g_host.lastCpuPixels = pixels;
    g_host.lastCpuW = w;
    g_host.lastCpuH = h;
    // The buffer lives until the server is done with it.
#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3dTex =
        uploadD3D11Frame(pixels->data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h), frame);
    frame.d3d11 = d3dTex.Get();
    g_host.server->submit(frame, [pixels, d3dTex] {});
#else
    g_host.server->submit(frame, [pixels] {});
#endif
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
        // The text lane: each request is an agent control command line, run
        // by the same dispatcher a local bro-ctl reaches (the display's,
        // under DRM), so every command works remotely with no code of its
        // own. The reply goes back from whatever thread finishes it.
        engine::ControlServer* control = &g_host.engine->control();
        server->set_text_handler([control](broremote::TextRequest req) {
            // Who sent it, for the agent inbox to stamp and log.
            engine::ControlOrigin origin;
            origin.via = "remote";
            origin.pid = req.peer_pid;
            origin.user = std::move(req.peer_user);
            origin.sshClient = std::move(req.peer_ssh_client);
            origin.client = std::move(req.client);
            control->post(
                std::move(req.argv), req.out_arg,
                [reply = std::move(req.reply)](bool ok, std::string payload, std::string file) {
                    reply(ok, std::move(payload), std::move(file));
                },
                std::move(origin));
        });
        LOG_INFO("bro.remote: hosting on %s", server->socket_path().c_str());
        return;
    }
    // The server is about to go: nothing may reach it after this returns.
    if (g_host.server) g_host.server->set_text_handler({});
    setFeeding(false);
    g_host.server = nullptr;
    LOG_INFO("bro.remote: stopped hosting");
}

}  // namespace

void installRemoteHost(engine::Engine& engine) {
    static engine::Engine* s_hookedEngine = nullptr;
    g_host.engine = &engine;
    if (s_hookedEngine != &engine) {
        s_hookedEngine = &engine;
        broremote::api::HostHooks hooks;
        hooks.serverChanged = &serverChanged;
        broremote::api::setHostHooks(std::move(hooks));
        engine.addFramePump(&pump);
        engine.addShutdownHook([eng = &engine] {
            broremote::api::shutdownRemote();
            if (s_hookedEngine == eng) s_hookedEngine = nullptr;
            if (g_host.engine == eng) {
                g_host.engine = nullptr;
#if defined(_WIN32)
                g_host.d3dTextures[0].Reset();
                g_host.d3dTextures[1].Reset();
                g_host.d3dContext.Reset();
                g_host.d3dDevice.Reset();
                g_host.d3dWidth = g_host.d3dHeight = 0;
                g_host.d3dSlotValid[0] = g_host.d3dSlotValid[1] = false;
#endif
            }
        });
        // The viewer side: <remoteview> and bro.remote.connect()'s sessions.
        installRemoteViewHost(engine);
    }
    broremote::api::installRemote();
}

}  // namespace bro::bronze_host
