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

#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "util/interrupt.h"
#include "util/log.h"

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
