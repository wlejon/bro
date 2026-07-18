// Secondary window hosts — the engine side of bro.window.open().
//
// Each host owns a real OS window (platform::Window::createSecondary —
// SDL_WINDOW_OPENGL, NO GL context) AND the isolated document realm rendered
// into it: its own JSContext, timers, DOM tree and 2D canvas scenes, built from
// `opts.src` by the shared sub-document core (engine/sub_document.h) that also
// backs <iframe>. Per frame the host document records on the main thread,
// replays into a window-sized GPU surface on the raster thread, and composites
// as one fullscreen quad on its own drawable.
//
// GL: there is exactly ONE GL context. compositeWindowHosts() makes it current
// on each host's drawable in turn, draws that host's single texture, and swaps
// at interval 0 — the main window keeps the frame's one pacing swap. Because
// each host publishes exactly ONE texture (no layer lists), the frame's single
// GLsync fence covers every host's sampling with no extra handshake.
//
// Lifecycle discipline: creation and destruction are QUEUED and drained at
// the raster-idle point (processPendingWindowHosts — beside
// processPendingIframeReloads in the frame loop, and in headless flush()).
// That is the one point where future chunks can tear down a host's document
// and GPU surfaces without racing the raster thread, and it keeps 'close'
// callbacks (which run app JS) off the input-event stack.

#include "engine/engine.h"
#include "engine/config_loader.h"
#include "engine/sub_document.h"

#include "dom/document.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/message_serializer.h"
#include "js/window_host_bindings.h"
#include "canvas/canvas_scene.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "render/command_buffer.h"
#include "render/gl_context.h"
#include "util/interrupt.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <algorithm>
#include <cstddef>

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
        // The keyboard cannot stay pointed at a window that is going away.
        if (focusedHostId_ == id) focusedHostId_ = 0;
        // Document first (frees JS/DOM state the raster thread replays), then
        // the surface into the owning context's free list, then the window.
        teardownWindowHostDoc(*h);
        queueIframeSurfaceFree(std::move(h->surface));
        h->surfW = h->surfH = 0;
        h->fboTexture = 0;
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
                              : static_cast<double>(h->window->getDisplayScale());
        h->boxW = w;
        h->boxH = ht;
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
        // The host document, as one fullscreen quad. The surface is a top-down
        // Skia GPU surface (V=0 at top), like the iframe layers in
        // compositeLayers. Quad coordinates are in the host's WINDOW units and
        // the shader's viewport uniform matches, so a HiDPI drawable simply
        // scales — v1 keeps host surfaces at window-size units.
        if (h->fboTexture) {
            float qw = static_cast<float>(std::max(1, h->boxW));
            float qh = static_cast<float>(std::max(1, h->boxH));
            glUseProgram(gl_->textureProgram());
            float vp[2] = {qw, qh};
            glUniform2fv(gl_->textureViewportLoc(), 1, vp);
            glUniform1i(gl_->textureSamplerLoc(), 0);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glBindVertexArray(uiQuadVAO_);
            glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                                  sizeof(render::TextureVertex), (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                                  sizeof(render::TextureVertex),
                                  (void*)offsetof(render::TextureVertex, u));
            glActiveTexture(GL_TEXTURE0);
            render::TextureVertex quad[6] = {
                {0,  0,  0, 0}, {qw, 0,  1, 0}, {qw, qh, 1, 1},
                {0,  0,  0, 0}, {qw, qh, 1, 1}, {0,  qh, 0, 1},
            };
            glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
            glBindTexture(GL_TEXTURE_2D, h->fboTexture);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
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
    for (auto& h : windowHosts_) {
        ids.push_back(h->id);
        teardownWindowHostDoc(*h);
        // The surface belongs to whichever context replayed it. Windowed, the
        // raster thread already released it on its way out (rasterThreadFunc's
        // exit cleanup runs before shutdown() gets here) and this is a no-op.
        // Headless, the main renderer owns it and ~Engine drains the queue
        // right after shutdown() returns.
        queueIframeSurfaceFree(std::move(h->surface));
        h->surfW = h->surfH = 0;
        h->fboTexture = 0;
    }
    windowHosts_.clear();  // destroys the SDL windows
    focusedHostId_ = 0;
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
        // The realm's innerWidth/innerHeight + 'resize' event follow at the
        // next record (syncWindowHostBox) — that runs at the raster-idle point,
        // so the app JS a resize listener triggers never lands mid-frame.
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
// raster-idle drain only (processPendingWindowHosts). Leaves h.document null if
// the realm can't be built; the caller treats that like a failed window create,
// so JS gets a clean 'close' instead of a live handle onto a blank window.
void Engine::createWindowHostDoc(WindowHost& h, SubDocSource& source) {
    SubDocRef ref = windowHostSubDoc(h);
    buildSubDocDocument(ref, source, effectiveColorScheme());

    SubDocRealmOptions ropts;
    // The host realm's window globals describe ITS window, not the primary one:
    // window.screen, navigator, and the scoped bro.window.* below all resolve
    // through the per-JSContext state window_bindings now keeps.
    ropts.window = h.window.get();
    ropts.displayScale = h.displayScale;
    ropts.headless = displayMode_ == DisplayMode::Headless;
    ropts.settings = settings_.get();
    ropts.installBroWindow = true;
    ropts.warnOnWebGL = true;
    ropts.nowMs = engineNowMs_;
    ropts.what = "bro.window";
    buildSubDocRealm(ref, jsRuntime_.get(), this, source, renderer_.get(), ropts);

    runSubDocScripts(ref, source, "bro.window");
    finishSubDocLoad(ref, source, renderer_.get(), audioEngine_.get(), *textMetrics_);
    // v1 refusal: syncIframes() only walks the app document, so a nested
    // <iframe> would silently never load. Say so.
    warnNestedIframes(ref, "bro.window");

    LOG_INFO("bro.window: loaded document '%s' (%dx%d, id=%llu)",
             source.appDir.c_str(), h.boxW, h.boxH,
             static_cast<unsigned long long>(h.id));

    // 'load' on the PARENT-side handle, once the document is parsed, scripted
    // and laid out — the same point <iframe> fires its element 'load'. Runs on
    // the parent realm's context (the handle lives there).
    h.loadFired = true;
    if (jsRuntime_)
        js::windowHostNotifyLoaded(jsRuntime_->getContext(), h.id);
}

void Engine::teardownWindowHostDoc(WindowHost& h) {
    if (!h.jsCtx && !h.document) return;
    h.hoveredElement = nullptr;
    h.activeElement = nullptr;
    // Surface deliberately untouched — see teardownSubDoc; every caller routes
    // it through queueIframeSurfaceFree.
    teardownSubDoc(windowHostSubDoc(h));
}

// Track the OS window's client size. SDL_EVENT_WINDOW_RESIZED already updated
// width/height; this is where the layout box and the realm catch up.
void Engine::syncWindowHostBox(WindowHost& h) {
    int w = std::max(1, h.width);
    int ht = std::max(1, h.height);
    if (w == h.boxW && ht == h.boxH) return;
    h.boxW = w;
    h.boxH = ht;
    // The parent observes the same resize on its handle ('resize' with
    // width/height), so an app can react to the user dragging a tool window's
    // edge without the child having to relay it.
    if (jsRuntime_)
        js::windowHostNotifyResized(jsRuntime_->getContext(), h.id, w, ht);
    if (h.document) {
        h.document->setMediaViewport(static_cast<float>(w), static_cast<float>(ht));
        h.document->markDirty();
    }
    if (!h.jsCtx) return;
    // Per-realm innerWidth/innerHeight + a 'resize' event — the same bridge the
    // app realm gets in handleResize().
    JSValue global = JS_GetGlobalObject(h.jsCtx);
    JS_SetPropertyStr(h.jsCtx, global, "innerWidth", JS_NewInt32(h.jsCtx, w));
    JS_SetPropertyStr(h.jsCtx, global, "innerHeight", JS_NewInt32(h.jsCtx, ht));
    JS_SetPropertyStr(h.jsCtx, global, "outerWidth", JS_NewInt32(h.jsCtx, w));
    JS_SetPropertyStr(h.jsCtx, global, "outerHeight", JS_NewInt32(h.jsCtx, ht));
    JSValue dispatch = JS_GetPropertyStr(h.jsCtx, global,
                                         "__bro_dispatch_window_event");
    if (JS_IsFunction(h.jsCtx, dispatch)) {
        JSValue evtType = JS_NewString(h.jsCtx, "resize");
        JSValue evt = JS_NewObject(h.jsCtx);
        JS_SetPropertyStr(h.jsCtx, evt, "type", JS_NewString(h.jsCtx, "resize"));
        JS_SetPropertyStr(h.jsCtx, evt, "target", JS_DupValue(h.jsCtx, global));
        JSValue dArgs[2] = {evtType, evt};
        JSValue ret = JS_Call(h.jsCtx, dispatch, global, 2, dArgs);
        JS_FreeValue(h.jsCtx, ret);
        JS_FreeValue(h.jsCtx, evtType);
        JS_FreeValue(h.jsCtx, evt);
    }
    JS_FreeValue(h.jsCtx, dispatch);
    JS_FreeValue(h.jsCtx, global);
    if (jsRuntime_) jsRuntime_->executePendingJobs();
}

// Advance every host document's timers + rAF, and report whether any needs
// (re)recording this frame. Same role tickIframes plays for <iframe>: host
// activity has no other route to uiDirty_, so without it an animating secondary
// window would never re-record.
// ---------------------------------------------------------------------------
// Messaging (see the header block on postMessageToWindowHost)
// ---------------------------------------------------------------------------

namespace {

// Fire a 'message' event at a host realm's window: addEventListener('message')
// listeners through the window polyfill's dispatcher, plus window.onmessage for
// the classic handler property. Takes ownership of `data`.
void dispatchRealmMessage(JSContext* ctx, JSValue data) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue evt = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, evt, "type", JS_NewString(ctx, "message"));
    JS_SetPropertyStr(ctx, evt, "data", data);  // takes ownership
    JS_SetPropertyStr(ctx, evt, "target", JS_DupValue(ctx, global));

    JSValue dispatch = JS_GetPropertyStr(ctx, global,
                                         "__bro_dispatch_window_event");
    if (JS_IsFunction(ctx, dispatch)) {
        JSValue type = JS_NewString(ctx, "message");
        JSValue args[2] = {type, evt};
        JSValue ret = JS_Call(ctx, dispatch, global, 2, args);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, type);
    }
    JS_FreeValue(ctx, dispatch);

    JSValue onmessage = JS_GetPropertyStr(ctx, global, "onmessage");
    if (JS_IsFunction(ctx, onmessage)) {
        JSValue ret = JS_Call(ctx, onmessage, global, 1, &evt);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, onmessage);

    JS_FreeValue(ctx, evt);
    JS_FreeValue(ctx, global);
}

} // namespace

uint64_t Engine::windowHostIdForContext(JSContext* ctx) const {
    if (!ctx) return 0;
    for (auto& h : windowHosts_)
        if (h->jsCtx == ctx) return h->id;
    return 0;
}

bool Engine::postMessageToWindowHost(uint64_t id, std::unique_ptr<js::Message> msg) {
    WindowHost* host = windowHostById(id);
    if (!host || host->pendingClose) return false;
    host->inbox.push_back(std::move(msg));
    uiDirty_ = true;  // make sure a frame reaches the drain point
    return true;
}

void Engine::postMessageToParent(uint64_t hostId, std::unique_ptr<js::Message> msg) {
    hostToParentMessages_.emplace_back(hostId, std::move(msg));
    uiDirty_ = true;
}

void Engine::drainWindowHostMessages() {
    if (!jsRuntime_) return;

    // Children first. Each host's inbox is swapped out before dispatch so a
    // handler that posts back to its own window queues for the NEXT drain
    // instead of extending this loop forever.
    for (auto& hptr : windowHosts_) {
        WindowHost* h = hptr.get();
        if (h->inbox.empty()) continue;
        std::vector<std::unique_ptr<js::Message>> batch;
        batch.swap(h->inbox);
        // A window that closed (or failed to build a realm) between post and
        // drain drops its mail — resolving the destination at DELIVERY time is
        // what makes a message racing teardown a no-op rather than a crash.
        if (!h->jsCtx || h->pendingClose) continue;
        JSContext* ctx = h->jsCtx;
        for (auto& m : batch) {
            JSValue data = js::deserializeMessage(ctx, *m);
            if (JS_IsException(data)) {
                js::Runtime::checkException(ctx, data);
                continue;
            }
            dispatchRealmMessage(ctx, data);
        }
    }

    // Then the parent side, in global post order — taken AFTER the child pass
    // so a reply posted from a child's 'message' handler lands in this same
    // drain (one round trip per drain, in both directions).
    if (!hostToParentMessages_.empty()) {
        std::vector<std::pair<uint64_t, std::unique_ptr<js::Message>>> batch;
        batch.swap(hostToParentMessages_);
        JSContext* ctx = jsRuntime_->getContext();
        for (auto& [hostId, m] : batch) {
            JSValue data = js::deserializeMessage(ctx, *m);
            if (JS_IsException(data)) {
                js::Runtime::checkException(ctx, data);
                continue;
            }
            // Unknown ids are a no-op inside the binding (handle already gone).
            js::windowHostNotifyMessage(ctx, hostId, data);
        }
    }
    jsRuntime_->executePendingJobs();
}

bool Engine::tickWindowHosts(double nowMs) {
    if (windowHosts_.empty()) return false;
    bool active = false;
    for (auto& h : windowHosts_) {
        if (!h->document || h->pendingClose) continue;
        if (tickSubDoc(windowHostSubDoc(*h), nowMs)) active = true;
    }
    if (jsRuntime_) jsRuntime_->executePendingJobs();
    return active;
}

} // namespace bro::engine
