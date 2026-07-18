// Secondary window hosts — the engine side of bro.window.open().
//
// v1 IN PROGRESS (multiwindow plan, chunk 1): each host owns a real OS window
// (platform::Window::createSecondary — SDL_WINDOW_OPENGL, NO GL context) with
// full open/close lifecycle, per-host focus/minimized/occluded state, and a
// per-window composite pass that clears it to the host's color and swaps at
// interval 0. The host's document/realm and actual content rendering land
// with the next chunk; until then `opts.src` is stored, not loaded.
//
// Lifecycle discipline: creation and destruction are QUEUED and drained at
// the raster-idle point (processPendingWindowHosts — beside
// processPendingIframeReloads in the frame loop, and in headless flush()).
// That is the one point where future chunks can tear down a host's document
// and GPU surfaces without racing the raster thread, and it keeps 'close'
// callbacks (which run app JS) off the input-event stack.

#include "engine/engine.h"

#include "js/runtime.h"
#include "js/window_host_bindings.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>

namespace bro::engine {

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

uint64_t Engine::openWindowHost(const WindowHostOptions& opts) {
    // No primary window (Server mode, or --no-gpu headless where SDL video
    // was never initialized) — there is nothing to share a swap chain with.
    // The binding turns 0 into a clean JS error.
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
        h->window.reset();  // destroys the SDL window (no GL context to touch)
        windowHosts_.erase(windowHosts_.begin() + static_cast<ptrdiff_t>(i));
        // Fire the handle's 'close' AFTER the registry forgot the id, so
        // handle.closed reads true inside the listener. Runs app JS — legal
        // here (the drain point runs other JS too, e.g. iframe reload load
        // events) and never on the input-event stack.
        if (jsRuntime_)
            js::windowHostNotifyClosed(jsRuntime_->getContext(), id);
    }

    // Creates. A create that fails closes the handle the same way an OS
    // close would (registry entry removed + 'close' fired), so JS never
    // holds a forever-pending window.
    std::vector<uint64_t> failed;
    for (auto& hptr : windowHosts_) {
        WindowHost* h = hptr.get();
        if (!h->pendingCreate) continue;
        h->pendingCreate = false;

        platform::Window::SecondaryConfig cfg;
        cfg.title = h->opts.title;
        cfg.width = static_cast<uint32_t>(h->opts.width);
        cfg.height = static_cast<uint32_t>(h->opts.height);
        cfg.hidden = h->opts.hidden;
        cfg.resizable = h->opts.resizable;
        cfg.borderless = h->opts.borderless;
        cfg.alwaysOnTop = h->opts.alwaysOnTop;
        cfg.x = h->opts.x;  // kWindowPosUnset == SecondaryConfig::kPosUnset (INT_MIN)
        cfg.y = h->opts.y;
        if (h->opts.display >= 0 && window_) {
            auto displays = window_->getDisplays();
            if (h->opts.display < static_cast<int>(displays.size())) {
                cfg.displayId = displays[static_cast<size_t>(h->opts.display)].id;
            } else {
                LOG_WARN("bro.window.open: display=%d, but only %zu display(s) attached",
                         h->opts.display, displays.size());
            }
        }

        h->window = platform::Window::createSecondary(cfg);
        if (!h->window) {
            LOG_ERROR("bro.window.open: secondary window creation failed (id=%llu)",
                      static_cast<unsigned long long>(h->id));
            failed.push_back(h->id);
            continue;
        }
        h->sdlId = h->window->windowId();
        int w = 0, ht = 0;
        h->window->getSize(w, ht);
        h->width = w;
        h->height = ht;
        LOG_INFO("bro.window: opened secondary window id=%llu sdl=%u (%dx%d%s)",
                 static_cast<unsigned long long>(h->id), h->sdlId, w, ht,
                 h->opts.hidden ? ", hidden" : "");
    }
    for (uint64_t id : failed) {
        for (size_t i = 0; i < windowHosts_.size(); ++i) {
            if (windowHosts_[i]->id == id) {
                windowHosts_.erase(windowHosts_.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
        if (jsRuntime_)
            js::windowHostNotifyClosed(jsRuntime_->getContext(), id);
    }
}

void Engine::compositeWindowHosts() {
    if (!window_ || !gl_) return;
    if (!anyPresentableWindowHosts()) return;

    SDL_GLContext mainCtx = window_->getGLContext();
    bool switched = false;
    for (auto& h : windowHosts_) {
        if (!h->window || h->pendingClose || h->minimized) continue;
        if (!h->window->makeGLCurrent(mainCtx)) continue;
        switched = true;
        // Secondary swaps must never block the frame: interval 0 for every
        // one of them; the main swap (last, below in the frame loop) is the
        // frame's single pacing swap. Re-set after every MakeCurrent — the
        // interval is per-context state on WGL but per-drawable on GLX, and
        // the call is cheap.
        SDL_GL_SetSwapInterval(0);
        int pw = 0, ph = 0;
        h->window->getSizeInPixels(pw, ph);
        if (pw <= 0 || ph <= 0) continue;
        glViewport(0, 0, pw, ph);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(h->clearColor[0], h->clearColor[1],
                     h->clearColor[2], h->clearColor[3]);
        glClear(GL_COLOR_BUFFER_BIT);
        // v1: blank clear only. The host document's composite (fboTexture
        // quad) lands with the next multiwindow chunk.
        h->window->swapWindow();
    }
    if (switched) {
        // Back to the main drawable with the configured vsync interval; the
        // frame loop's WebGL restoreState + main swap follow. Our GL state
        // touches above (viewport/clear color/scissor) are re-established by
        // the next frame's composite setup and by restoreState for WebGL apps.
        window_->makeGLCurrent(mainCtx);
        window_->applySwapIntervalPreference();
    }
}

void Engine::destroyAllWindowHosts(bool notifyJs) {
    if (windowHosts_.empty()) return;
    std::vector<uint64_t> ids;
    ids.reserve(windowHosts_.size());
    for (auto& h : windowHosts_) ids.push_back(h->id);
    windowHosts_.clear();  // destroys the SDL windows
    if (notifyJs && jsRuntime_) {
        for (uint64_t id : ids)
            js::windowHostNotifyClosed(jsRuntime_->getContext(), id);
    }
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
        uiDirty_ = true;  // repaint the host at its new size
    }
}

void Engine::handleHostFocusChanged(uint32_t sdlWindowId, bool focused) {
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        host->focused = focused;
    }
}

void Engine::handleHostMinimized(uint32_t sdlWindowId, bool minimized) {
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        host->minimized = minimized;
        if (!minimized) uiDirty_ = true;  // present a fresh frame on restore
    }
}

void Engine::handleHostOccluded(uint32_t sdlWindowId, bool occluded) {
    if (WindowHost* host = windowHostBySdlId(sdlWindowId)) {
        host->occluded = occluded;
    }
}

} // namespace bro::engine
