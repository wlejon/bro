// Secondary window hosts — the engine side of secondary windows.
//
// Each host owns a real OS window (platform::createWindow, with
// the primary window's graphics backend) AND the isolated document rendered
// into it: its own DOM tree and 2D canvas scenes, built from
// `opts.src` by the shared sub-document core (engine/sub_document.h) that also
// backs <iframe>. Per frame the host document records on the main thread,
// replays into a window-sized surface on the raster thread, which publishes
// a snapshot of it (WindowHost::published).
//
// Presentation: every host has its own VulkanSwapchain + VulkanPresenter on
// the one shared VulkanContext. compositeWindowHosts() draws the published
// frame over the host's clear color and presents it, without vsync — the main
// window keeps the frame's one paced present. All of them submit through the
// context's queue owner within the same frame of its frame ring.
//
// Lifecycle discipline: creation and destruction are QUEUED and drained at
// the raster-idle point (processPendingWindowHosts — beside
// processPendingIframeReloads in the frame loop, and in headless flush()).
// That is the one point where future chunks can tear down a host's document
// and GPU surfaces without racing the raster thread.

#include "engine/engine.h"
#include "engine/config_loader.h"
#include "engine/sub_document.h"
#include "bronze_host/host_window_open.h"

#include "dom/document.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "canvas/canvas_scene.h"
#include "platform/event_loop.h"
#include "platform/window.h"
#include "render/command_buffer.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#include "render/vulkan_swapchain.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <exception>

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cstddef>

namespace bro::engine {

WindowHost::WindowHost() = default;
WindowHost::~WindowHost() = default;

Engine::WindowHost* Engine::windowHostById(uint64_t id) {
    for (auto& h : windowHosts_)
        if (h->id == id) return h.get();
    return nullptr;
}

Engine::WindowHost* Engine::windowHostBySdlId(uint32_t sdlId) {
    if (sdlId == 0) return nullptr;
    for (auto& h : windowHosts_)
        if (h->sdlId == sdlId) return h.get();
    return nullptr;
}

bool Engine::anyLiveWindowHosts() const {
    // Counts every host whose SDL window exists — pendingClose included,
    // because SDL still counts that window when deciding whether a main-
    // window close request is "the last window" (and thus whether it will
    // follow up with SDL_EVENT_QUIT). See handleWindowCloseRequested.
    for (auto& h : windowHosts_)
        if (h->window) return true;
    return false;
}

bool Engine::anyPresentableWindowHosts() const {
    for (auto& h : windowHosts_)
        if (h->window && !h->pendingClose && !h->minimized) return true;
    return false;
}

bool Engine::anyWindowHostFocused() const {
    for (auto& h : windowHosts_)
        if (h->window && h->focused) return true;
    return false;
}

WindowHost* Engine::windowHostForDocument(const dom::Document* doc) {
    if (!doc) return nullptr;
    for (auto& h : windowHosts_) {
        if (h && h->document.get() == doc) return h.get();
    }
    return nullptr;
}

bool Engine::isWindowHostDocument(const dom::Document* doc) const {
    if (!doc) return false;
    for (auto& h : windowHosts_) {
        if (h && h->document.get() == doc) return true;
    }
    return false;
}

uint64_t Engine::openWindowHost(const WindowHostOptions& opts) {
    // No primary window (Server mode, or --no-gpu headless where SDL video
    // was never initialized) — there is nothing to share a swap chain with.
    if (!window_) return 0;

    auto host = std::make_unique<WindowHost>();
    host->id = nextWindowHostId_++;
    host->opts = opts;
    // Headless policy: secondary windows are always hidden so a test can
    // never pop OS windows over the desk the suite runs on (and their scale
    // stays pinned with the rest of the headless pipeline).
    if (displayMode_ == DisplayMode::Headless) host->opts.hidden = true;
    host->width = host->opts.width;
    host->height = host->opts.height;
    uint64_t id = host->id;
    windowHosts_.push_back(std::move(host));
    // Reach the raster-idle drain even if the app is otherwise idle.
    uiDirty_ = true;
    return id;
}

void Engine::processPendingWindowHosts() {
    // Closes first: an open() + close() issued before any drain never
    // materializes an OS window — the handle just closes cleanly.
    for (size_t i = 0; i < windowHosts_.size();) {
        WindowHost* h = windowHosts_[i].get();
        if (!h->pendingClose) { ++i; continue; }
        uint64_t id = h->id;
        // The keyboard cannot stay pointed at a window that is going away.
        if (focusedHostId_ == id) focusedHostId_ = 0;
        // Document first (frees DOM state the raster thread replays), then
        // the surface into the owning context's free list, then the window.
        teardownWindowHostDoc(*h);
        queueIframeSurfaceFree(std::move(h->surface));
        queueIframeSurfaceFree(std::move(h->spare));
        h->surfW = h->surfH = 0;
        h->published.clear();
        h->presenter.reset();  // the swapchain's surface must go before its window
        h->swapchain.reset();
        h->window.reset();
        windowHosts_.erase(windowHosts_.begin() + static_cast<ptrdiff_t>(i));
        bro::bronze_host::windowHostNotifyClosed(id);
    }

    // Creates. A create that fails closes the handle the same way an OS
    // close would (registry entry removed), so no forever-pending window.
    std::vector<uint64_t> failed;
    for (auto& hptr : windowHosts_) {
        WindowHost* h = hptr.get();
        if (!h->pendingCreate) continue;
        h->pendingCreate = false;

        // Load the child app BEFORE opening its OS window: a src that doesn't
        // resolve should never flash an empty window, and the app's own
        // bro.json is what fills in the window options open() left unset.
        SubDocSource source = loadSubDocSource(manifest_.basePath, h->opts.src,
                                               &assetMounts_, "bro.window.open");
        if (!source.ok) {
            failed.push_back(h->id);
            continue;
        }
        applyChildManifestDefaults(*h, source.appDir);

        platform::WindowConfig cfg;
        cfg.vsync = false;
        cfg.title = h->opts.title;
        cfg.width = static_cast<uint32_t>(h->opts.width);
        cfg.height = static_cast<uint32_t>(h->opts.height);
        cfg.hidden = h->opts.hidden;
        cfg.resizable = h->opts.resizable;
        cfg.borderless = h->opts.borderless;
        cfg.alwaysOnTop = h->opts.alwaysOnTop;
        cfg.x = h->opts.x;  // kWindowPosUnset == WindowConfig::kPosUnset (INT_MIN)
        cfg.y = h->opts.y;
        cfg.backend = window_->backend();
        if (h->opts.display >= 0 && window_) {
            auto displays = window_->getDisplays();
            if (h->opts.display < static_cast<int>(displays.size())) {
                cfg.displayId = displays[static_cast<size_t>(h->opts.display)].id;
            } else {
                LOG_WARN("bro.window.open: display=%d, but only %zu display(s) attached",
                         h->opts.display, displays.size());
            }
        }

        auto tryCreate = [](const platform::WindowConfig& c) -> std::unique_ptr<platform::Window> {
            try {
                return platform::createWindow(c);
            } catch (const std::exception& e) {
                LOG_ERROR("bro.window.open: %s", e.what());
                return nullptr;
            }
        };
        h->window = tryCreate(cfg);
        if (!h->window && cfg.backend != platform::GraphicsBackend::Software) {
            cfg.backend = platform::GraphicsBackend::Software;
            h->window = tryCreate(cfg);
        }
        if (!h->window) {
            LOG_ERROR("bro.window.open: secondary window creation failed (id=%llu)",
                      static_cast<unsigned long long>(h->id));
            failed.push_back(h->id);
            continue;
        }
        h->sdlId = h->window->windowId();
        // Resize limits (bro.json minWidth/… or explicit opts) — applied after
        // creation, like bro.window.setMinSize does for the primary window.
        if (h->opts.minWidth > 0 || h->opts.minHeight > 0)
            h->window->setMinimumSize(h->opts.minWidth, h->opts.minHeight);
        if (h->opts.maxWidth > 0 || h->opts.maxHeight > 0)
            h->window->setMaximumSize(h->opts.maxWidth, h->opts.maxHeight);
        int w = 0, ht = 0;
        h->window->getSize(w, ht);
        h->width = w;
        h->height = ht;
        // Headless pins the scale like the rest of the pipeline; windowed
        // hosts get the scale of the display they actually opened on.
        h->displayScale = (displayMode_ == DisplayMode::Headless)
                              ? 1.0
                              : static_cast<double>(h->window->getDevicePixelRatio());
        h->boxW = w;
        h->boxH = ht;
        createWindowHostPresenter(*h);
        LOG_INFO("bro.window: opened secondary window id=%llu sdl=%u (%dx%d%s)",
                 static_cast<unsigned long long>(h->id), h->sdlId, w, ht,
                 h->opts.hidden ? ", hidden" : "");
        createWindowHostDoc(*h, source);
        if (!h->document) failed.push_back(h->id);
    }
    for (uint64_t id : failed) {
        for (size_t i = 0; i < windowHosts_.size(); ++i) {
            if (windowHosts_[i]->id == id) {
                teardownWindowHostDoc(*windowHosts_[i]);
                queueIframeSurfaceFree(std::move(windowHosts_[i]->surface));
                queueIframeSurfaceFree(std::move(windowHosts_[i]->spare));
                windowHosts_.erase(windowHosts_.begin() + static_cast<ptrdiff_t>(i));
                bro::bronze_host::windowHostNotifyClosed(id);
                break;
            }
        }
    }
}

// A windowed host on the Vulkan context gets its own swapchain and presenter.
// Headless hosts are never presented (capture() renders them on demand), and
// a software-backend host presents through its window's framebuffer.
void Engine::createWindowHostPresenter(WindowHost& h) {
    if (displayMode_ != DisplayMode::Windowed || !vulkanContext_ || !h.window ||
        h.window->backend() != platform::GraphicsBackend::Vulkan)
        return;
    auto swapchain = std::make_unique<render::VulkanSwapchain>(*vulkanContext_, h.window.get(),
                                                               /*vsync=*/false);
    if (!swapchain->init()) {
        LOG_ERROR("bro.window: no swapchain for secondary window id=%llu; it will stay blank",
                  static_cast<unsigned long long>(h.id));
        return;
    }
    auto presenter = std::make_unique<render::VulkanPresenter>(*vulkanContext_, *swapchain);
    if (!presenter->init()) {
        LOG_ERROR("bro.window: no presenter for secondary window id=%llu; it will stay blank",
                  static_cast<unsigned long long>(h.id));
        return;
    }
    presenter->setCapturePresents(capturePresentsRequested());
    h.swapchain = std::move(swapchain);
    h.presenter = std::move(presenter);
}

// Present each host's last published frame over its clear color. Runs on the
// main thread after the frame's raster handshake; the published frame is the
// only thing it reads of the raster thread's work.
void Engine::compositeWindowHosts() {
    if (!window_ || displayMode_ != DisplayMode::Windowed) return;
    if (!anyPresentableWindowHosts()) return;

    for (auto& h : windowHosts_) {
        if (!h->window || h->pendingClose || h->minimized) continue;
        if (!h->presenter && h->window->backend() != platform::GraphicsBackend::Software) continue;
        int pw = 0, ph = 0;
        h->window->getSizeInPixels(pw, ph);
        if (pw <= 0 || ph <= 0) continue;

        // A GPU frame is sampled in place over the clear color — in device
        // px at the host's render scale, like the drawable: 1:1.
        if (render::SkiaImageRef image = h->published.gpu(); image && h->presenter) {
            render::PresentFrame frame;
            render::PresentImage& layer = frame.images.emplace_back(render::PresentImage::at1to1(
                image->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image->width, image->height));
            layer.view = image->view;
            std::copy(std::begin(h->clearColor), std::end(h->clearColor), std::begin(frame.clearColor));
            h->presenter->present(frame);
            h->presentSurface.reset();
            continue;
        }
        if (!h->presentSurface || h->presentSurface->width() != pw || h->presentSurface->height() != ph)
            h->presentSurface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(pw, ph));
        if (!h->presentSurface) continue;

        SkCanvas* canvas = h->presentSurface->getCanvas();
        canvas->clear(SkColor4f{h->clearColor[0], h->clearColor[1], h->clearColor[2], h->clearColor[3]});
        // The published frame is in device px at the host's render scale,
        // like the drawable: drawn 1:1.
        if (sk_sp<SkImage> frame = h->published.get()) canvas->drawImage(frame, 0.0f, 0.0f);

        if (h->presenter) {
            h->presenter->presentSurface(h->presentSurface.get());
        } else {
            SkPixmap pm;
            if (h->presentSurface->peekPixels(&pm))
                h->window->presentPixels(pm.addr(), pm.width(), pm.height(), static_cast<int>(pm.rowBytes()),
                                         pm.colorType() == kBGRA_8888_SkColorType);
        }
    }
}

void Engine::destroyAllWindowHosts() {
    if (windowHosts_.empty()) return;
    for (auto& h : windowHosts_) {
        uint64_t id = h->id;
        teardownWindowHostDoc(*h);
        queueIframeSurfaceFree(std::move(h->surface));
        queueIframeSurfaceFree(std::move(h->spare));
        h->surfW = h->surfH = 0;
        h->published.clear();
        bro::bronze_host::windowHostNotifyClosed(id);
    }
    windowHosts_.clear();  // destroys each host's presenter, swapchain, then SDL window
    focusedHostId_ = 0;
}

void Engine::closeWindowHost(uint64_t id) {
    WindowHost* host = windowHostById(id);
    if (!host || host->pendingClose) return;  // unknown or double-close: no-op
    host->pendingClose = true;
    // Make sure a frame actually reaches the drain point even if the app is
    // otherwise idle (same trick as reloadIframe).
    uiDirty_ = true;
}

void Engine::handleWindowCloseRequested(uint32_t sdlWindowId) {
    if (window_ && sdlWindowId == window_->windowId()) {
        // Main window. With ONLY the main window alive, SDL follows this
        // event with SDL_EVENT_QUIT and the event loop's quit path
        // (requestInterrupt + quit) handles it exactly as it always has —
        // acting here too would call requestInterrupt twice, and its second
        // call hard-exits the process. With secondary windows open SDL sends
        // no QUIT (the main window isn't the last one), so the close request
        // itself must run the quit path.
        if (anyLiveWindowHosts()) {
            ::bro::util::requestInterrupt();
            if (eventLoop_) eventLoop_->requestQuit();
            running_ = false;
        }
        return;
    }
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        // OS close button on a secondary window: same path as handle.close()
        // — the window destroys and the handle's 'close' event fires at the
        // next drain.
        closeWindowHost(host->id);
    }
}

void Engine::handleHostResized(uint32_t sdlWindowId, int w, int h) {
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        host->width = w;
        host->height = h;
        // The document's media viewport and 'resize' event follow at the
        // next record (syncWindowHostBox).
        uiDirty_ = true;  // repaint the host at its new size
    }
}

void Engine::handleHostFocusChanged(uint32_t sdlWindowId, bool focused) {
    WindowHost* host = windowHostBySdlId(sdlWindowId);
    if (!host) return;
    host->focused = focused;
    // focusedHostId_ is what keyboard / text input / IME follow. Only clear it
    // for the window that actually held it: focus moving A → B delivers B's
    // gain and A's loss in an unspecified order, and clearing on A's loss
    // after B's gain would strand the keyboard on no window at all.
    if (focused) focusedHostId_ = host->id;
    else if (focusedHostId_ == host->id) focusedHostId_ = 0;
    // Web semantics per realm: an unfocused window is not a hidden document,
    // but bro treats focus loss as backgrounding for the app realm already
    // (engine_frame.cpp), so hosts follow the same rule for consistency —
    // document.hidden flips and visibilitychange fires in THAT realm only.
    windowHostSetVisibility(*host, focused);
    if (!focused) {
        // Dropping the pointer state avoids a stuck :hover / half-finished
        // click streak when the pointer leaves with the focus.
        if (host->hoveredElement) {
            host->hoveredElement->markDirty();
            host->hoveredElement = nullptr;
            uiDirty_ = true;
        }
        host->pressedButtons = 0;
        host->controlDragElement.reset();
    }
}

void Engine::handleHostMinimized(uint32_t sdlWindowId, bool minimized) {
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        host->minimized = minimized;
        // A minimized window is a hidden document in that realm; restoring
        // makes it visible again. Timers keep ticking either way.
        windowHostSetVisibility(*host, !minimized);
        if (!minimized) uiDirty_ = true;  // present a fresh frame on restore
    }
}

void Engine::handleHostOccluded(uint32_t sdlWindowId, bool occluded) {
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        host->occluded = occluded;
    }
}

// ---------------------------------------------------------------------------
// The host's document realm
// ---------------------------------------------------------------------------

// Seed the window options the open() caller left unset from the child app's
// own bro.json — a palette app that declares `{"width":320,"borderless":true}`
// opens that way with a bare bro.window.open('palette'). Precedence:
// explicit open() options > the child's bro.json > the built-in defaults.
//
// Absence is detected by seeding an EngineConfig with the SAME defaults
// WindowHostOptions carries and letting parseConfig overwrite only the keys
// the file actually contains.
void Engine::applyChildManifestDefaults(WindowHost& h, const std::string& appDir) {
    if (appDir.empty()) return;
    EngineConfig cfg;
    cfg.title = "bro";
    cfg.graphics.width = 800;
    cfg.graphics.height = 600;
    cfg.graphics.resizable = true;
    cfg.graphics.borderless = false;
    cfg.graphics.alwaysOnTop = false;
    cfg.graphics.minWidth = cfg.graphics.minHeight = 0;
    cfg.graphics.maxWidth = cfg.graphics.maxHeight = 0;
    if (!parseConfig(appDir + "/bro.json", cfg)) return;

    const auto& p = h.opts.provided;
    if (!p.width)       h.opts.width       = cfg.graphics.width;
    if (!p.height)      h.opts.height      = cfg.graphics.height;
    if (!p.title)       h.opts.title       = cfg.title;
    if (!p.resizable)   h.opts.resizable   = cfg.graphics.resizable;
    if (!p.borderless)  h.opts.borderless  = cfg.graphics.borderless;
    if (!p.alwaysOnTop) h.opts.alwaysOnTop = cfg.graphics.alwaysOnTop;
    if (!p.minWidth)    h.opts.minWidth    = cfg.graphics.minWidth;
    if (!p.minHeight)   h.opts.minHeight   = cfg.graphics.minHeight;
    if (!p.maxWidth)    h.opts.maxWidth    = cfg.graphics.maxWidth;
    if (!p.maxHeight)   h.opts.maxHeight   = cfg.graphics.maxHeight;
    if (h.opts.width < 1) h.opts.width = 1;
    if (h.opts.height < 1) h.opts.height = 1;
    h.width = h.opts.width;
    h.height = h.opts.height;
}

// Build one host's document from the already-loaded `source`. Runs at the
// raster-idle drain only (processPendingWindowHosts).
void Engine::createWindowHostDoc(WindowHost& h, SubDocSource& source) {
    SubDocRef ref = windowHostSubDoc(h);
    buildSubDocDocument(ref, source, effectiveColorScheme(),
                        static_cast<float>(h.displayScale));

    runSubDocScripts(ref, source, this, /*isChild=*/true);

    finishSubDocLoad(ref, source, renderer_.get(), audioEngine_.get(), *textMetrics_);
    warnNestedIframes(ref, "bro.window");

    LOG_INFO("bro.window: loaded document '%s' (%dx%d, id=%llu)",
             source.appDir.c_str(), h.boxW, h.boxH,
             static_cast<unsigned long long>(h.id));

    h.loadFired = true;
    bro::bronze_host::windowHostNotifyLoaded(h.id);
}

void Engine::teardownWindowHostDoc(WindowHost& h) {
    if (!h.document) return;
    h.hoveredElement = nullptr;
    h.activeElement = nullptr;
    teardownSubDoc(windowHostSubDoc(h));
}

void Engine::syncWindowHostBox(WindowHost& h) {
    int w = std::max(1, h.width);
    int ht = std::max(1, h.height);
    if (w == h.boxW && ht == h.boxH) return;
    h.boxW = w;
    h.boxH = ht;
    bro::bronze_host::windowHostNotifyResized(h.id, w, ht);
    if (h.document) {
        h.document->setMediaViewport(static_cast<float>(w), static_cast<float>(ht));
        h.document->markDirty();
    }
    dom::Event resizeEvt("resize", /*bubbles=*/false, /*cancelable=*/false);
    resizeEvt.setIsTrusted(true);
    dom::dispatchWindowEvent(h.document.get(), resizeEvt);
}

bool Engine::tickWindowHosts(double nowMs) {
    if (windowHosts_.empty()) return false;
    bool active = false;
    for (auto& h : windowHosts_) {
        if (!h->document || h->pendingClose) continue;
        if (tickSubDoc(windowHostSubDoc(*h), nowMs)) active = true;
    }
    return active;
}

} // namespace bro::engine
