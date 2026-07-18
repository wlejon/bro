#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/ai_bindings.h"
#include "js/audio_scene_sync.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "js/net_bindings.h"
#include "js/web_animation_bindings.h"
#include "js/steam_bindings.h"
#include "js/worker.h"
#include "js/wake_bindings.h"
#include "js/gesture_bindings.h"
#include "js/kws_bindings.h"
#include "js/async_job.h"
#include "js/mic_bindings.h"
#include "layout/box.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "audio_inference/audio_inference.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "render/gl_context.h"
#include "render/skia_backend.h"

#include <broaudio/engine.h>
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include "observer_check.js.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace bro::engine {

double Engine::serverUptime() const {
    if (serverStartTime_ <= 0.0) return 0.0;
    return (util::currentTimeMs() - serverStartTime_) / 1000.0;
}

/// SDL event watcher — fires on the main thread during Windows' modal
/// move/resize loop, keeping JS timers (audio sequencer etc.) alive.
static bool modalEventWatcher(void* userdata, SDL_Event* event)
{
    if (event->type >= SDL_EVENT_WINDOW_FIRST &&
        event->type <= SDL_EVENT_WINDOW_LAST) {
        static_cast<Engine*>(userdata)->tickTimersOnly();
    }
    return true; // keep the event in the queue
}

void Engine::run() {
    // Headless mode: initial layout is done in constructor, return immediately.
    // The HeadlessController drives subsequent frames via advanceTime/flush.
    if (displayMode_ == DisplayMode::Headless) {
        // Resync virtual time to current wall clock so timers registered in
        // test scripts fire correctly relative to advanceTime() calls.
        // Without this, virtualTime_ (set early in the constructor) lags behind
        // the wall clock by the time system panels and fonts finish loading.
        virtualTime_ = util::currentTimeMs();
        // The bro.time scaled clock rebases with it — they advance in lockstep
        // through advanceTime (identical at scale 1, so pre-existing tests see
        // exactly the old timer behavior).
        engineNowMs_ = virtualTime_;
        // Splash elapsed is measured against virtualTime_, so rebase its start
        // too — otherwise elapsed would count the constructor time and the
        // splash would auto-dismiss partway through the first advanceTime().
        if (splashVisible_) splashStartMs_ = virtualTime_;
        timers_->tick(engineNowMs_);
        return;
    }

    // Server mode: tick loop driven by real wall-clock time.
    if (displayMode_ == DisplayMode::Server) {
        running_ = true;
        LOG_INFO("[server] Running at %.0f ticks/sec", serverTickRate_);

        while (running_ && !serverStopRequested_ && !bro::util::interrupted()) {
            double tickStart = util::currentTimeMs();
            double tickIntervalMs = 1000.0 / serverTickRate_;

            // Advance the bro.time scaled clock; timers run on it in every
            // display mode (pause freezes them, timescale stretches them).
            if (lastWallTickMs_ > 0.0 && tickStart > lastWallTickMs_)
                engineNowMs_ += (tickStart - lastWallTickMs_) * effectiveTimeScale();
            lastWallTickMs_ = tickStart;
            timers_->tick(engineNowMs_);

            auto pumpBrokit = [&](const char* fnName) {
                JSContext* ctx = jsRuntime_->getContext();
                JSValue global = JS_GetGlobalObject(ctx);
                JSValue tickFn = JS_GetPropertyStr(ctx, global, fnName);
                if (JS_IsFunction(ctx, tickFn)) {
                    JSValue ret = JS_Call(ctx, tickFn, JS_UNDEFINED, 0, nullptr);
                    JS_FreeValue(ctx, ret);
                }
                JS_FreeValue(ctx, tickFn);
                JS_FreeValue(ctx, global);
            };
            pumpBrokit("__brokit_fetch_tick");
            pumpBrokit("__brokit_ws_tick");
            pumpBrokit("__brokit_fs_watch_tick");

            // Reap finished audio voices off the audio thread.
            if (audioEngine_) audioEngine_->update();

            jsRuntime_->executePendingJobs();
            js::tickWorkers(jsRuntime_->getContext());
            // Deliver any results (wake/kws/gesture fires, mic chunks) the audio
            // subsystems published since last frame on this (main) thread. Each
            // pump is registered at install time only when its subsystem is built
            // — the worker drains the mic rings on its own clock, not from here.
            for (auto& pump : framePumps_) pump();
            js::tickAsync(jsRuntime_->getContext());
            jsRuntime_->executePendingJobs();

            if (steamService_) {
                js::SteamBindings::poll(jsRuntime_->getContext());
                jsRuntime_->executePendingJobs();
            }

            // Step physics (fixed timestep accumulator). Consume must run
            // unconditionally — it is the only Done→Idle transition, so gating
            // it on isIdle() would wedge the sim after the first step.
#if BRO_WITH_PHYSICS
            if (physicsWorld_) {
                physicsWorld_->consumeStep();
                if (physicsWorld_->isIdle()) {
                    double stepMs = physicsWorld_->timeStep() * 1000.0;
                    double nowPhys = util::currentTimeMs();
                    if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
                    // Wall delta scaled by bro.time — the fixed timestep is
                    // preserved; pause/timescale change how much sim time
                    // accumulates per wall second (Godot semantics).
                    physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_) *
                                       effectiveTimeScale();
                    lastPhysicsTimeMs_ = nowPhys;
                    if (physicsAccumMs_ >= stepMs) {
                        physicsAccumMs_ -= stepMs;
                        if (physicsAccumMs_ > stepMs * 3) physicsAccumMs_ = 0;
                        physicsWorld_->signalStep();
                    }
                }
            }
#endif

            // Periodic GC
            double now = util::currentTimeMs();
            if (now - lastGCMs_ >= kGCIntervalMs) {
                JS_RunGC(jsRuntime_->getRuntime());
                lastGCMs_ = now;
            }

            double elapsed = util::currentTimeMs() - tickStart;
            double sleepMs = tickIntervalMs - elapsed;
            if (sleepMs > 0.5) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<int64_t>(sleepMs * 1000.0)));
            }
        }

        LOG_INFO("[server] Stopped (uptime: %.1fs)", serverUptime());
        return;
    }

    running_ = true;

    // Wire up event-loop callbacks.
    eventLoop_->onQuit       = [this]() { running_ = false; };
    eventLoop_->onResize     = [this](uint32_t w, uint32_t h) { handleResize((int)w, (int)h); };
    eventLoop_->onMouseDown  = [this](float x, float y, uint8_t b) { handleMouseDown(x, y, (int)b); };
    eventLoop_->onMouseUp    = [this](float x, float y, uint8_t b) { handleMouseUp(x, y, (int)b); };
    eventLoop_->onMouseMove  = [this](float x, float y, float xr, float yr) { handleMouseMove(x, y, xr, yr); };
    eventLoop_->onKeyDown    = [this](int32_t k, int32_t s, uint16_t m, bool r) { handleKeyDown(k, s, (int)m, r); };
    eventLoop_->onKeyUp      = [this](int32_t k, int32_t s, uint16_t m, bool r) { handleKeyUp(k, s, (int)m, r); };
    eventLoop_->onTextInput  = [this](const std::string& t) { handleTextInput(t); };
    eventLoop_->onTextEditing = [this](const std::string& t, int32_t s, int32_t l) { handleTextEditing(t, s, l); };
    eventLoop_->onWheel      = [this](float x, float y, float dx, float dy) { handleWheel(x, y, dx, dy); };
    eventLoop_->onDropFile   = [this](const std::string& p, float x, float y) { handleDropFile(p, x, y); };
    eventLoop_->onDropText   = [this](const std::string& t, float x, float y) { handleDropText(t, x, y); };
    eventLoop_->onFingerDown = [this](uint64_t id, float x, float y, float p) { handleTouchDown(id, x, y, p); };
    eventLoop_->onFingerMove = [this](uint64_t id, float x, float y, float p) { handleTouchMove(id, x, y, p); };
    eventLoop_->onFingerUp   = [this](uint64_t id, float x, float y) { handleTouchUp(id, x, y); };
    eventLoop_->onFingerCancel = [this](uint64_t id, float x, float y) { handleTouchCancel(id, x, y); };
    eventLoop_->onGamepadAdded   = [this](uint32_t id) { handleGamepadAdded(id); };
    eventLoop_->onGamepadRemoved = [this](uint32_t id) { handleGamepadRemoved(id); };
    eventLoop_->onGamepadButton  = [this](uint32_t id, int b, bool down) { handleGamepadButton(id, b, down); };
    eventLoop_->onGamepadAxis    = [this](uint32_t id, int a, float v) { handleGamepadAxis(id, a, v); };
    // SDL drops relative mouse mode on focus loss on some platforms — keep our
    // engine-side lock state in sync so apps see a pointerlockchange.
    eventLoop_->onFocusLost   = [this]() { windowFocused_ = false; exitPointerLock(); setPageVisibility(false); };
    eventLoop_->onFocusGained = [this]() { windowFocused_ = true; setPageVisibility(true); };
    // Web semantics: a minimized window is a hidden document (document.hidden
    // flips + visibilitychange fires); restore/maximize make it visible again.
    // Focus loss usually lands first and already set hidden — the JS-side
    // __bro_set_visibility guard makes the repeat a no-op, so these only add
    // the transitions focus alone can't see (e.g. restore-without-focus).
    eventLoop_->onMinimized = [this]() { setPageVisibility(false); };
    eventLoop_->onMaximized = [this]() { setPageVisibility(true); };
    eventLoop_->onRestored  = [this]() { setPageVisibility(true); };
    // OS theme flip → re-evaluate @media (prefers-color-scheme) and restyle.
    // A no-op when appearance.colorScheme forces "light"/"dark".
    eventLoop_->onSystemThemeChanged = [this]() { applyColorScheme(); };
    // OS scaling change / moved to a display with a different scale →
    // refresh window.devicePixelRatio and fire a window resize event.
    eventLoop_->onDisplayScaleChanged = [this]() { handleDisplayScaleChanged(); };

    // Seed focus from the window's actual state — an app can launch unfocused
    // (e.g. spawned behind another window), and SDL won't emit a FOCUS_LOST for
    // a window that was never focused, so the event handlers alone can miss it.
    windowFocused_ =
        (SDL_GetWindowFlags(window_->getSDLWindow()) & SDL_WINDOW_INPUT_FOCUS) != 0;

    // Initial style + layout already ran in the Engine constructor (step 10a,
    // before DOMContentLoaded/load were dispatched, so apps can measure
    // geometry in those handlers). The main loop below re-layouts on demand
    // via dirty tracking, so there is nothing to lay out here.

    // Shared canvas-raster worker: one GL context + thread that rasterizes
    // every threaded canvas scene. Create it once here, while quiescent (before
    // the raster thread starts below), so canvas churn never creates/destroys a
    // GL context on the hot path — the create/destroy-vs-raster overlap was a
    // Windows/NVIDIA crash. Then bind any scenes already created during load.
    {
        auto ctx = window_->createSharedContext();
        if (ctx) {
            canvasRasterThread_ = std::make_unique<canvas::CanvasRasterThread>();
            canvasRasterThread_->start(ctx, window_->getSDLWindow());
        }
    }
    if (canvasRasterThread_ && canvasRasterThread_->started()) {
        for (auto& cs : canvasScenes_) {
            if (cs && !cs->isThreaded()) cs->bindRasterThread(canvasRasterThread_.get());
        }
    }

    // Raster thread: create its shared GL context on the main thread (macOS
    // /AppKit requirement), then block until the worker has MakeCurrent'd it
    // so no later wgl*Context call can overlap.
    rasterGLContext_ = window_->createSharedContext();
    if (!rasterGLContext_) {
        LOG_ERROR("Failed to create shared GL context for raster thread");
        return;
    }
    rasterReady_.store(false, std::memory_order_relaxed);

    // Spin up the synchronization owners, then the worker threads.
    framePresenter_  = std::make_unique<FramePresenter>();
    layoutPipeline_  = std::make_unique<LayoutPipeline>();

    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);
    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);

    // Event watcher keeps JS timers alive during Windows' modal move/resize loop.
    SDL_AddEventWatch(modalEventWatcher, this);

    // Helper to build a complete raster snapshot. Used everywhere we hand the
    // worker a frame so we can never accidentally signal with a partially
    // populated snapshot (the historical "no-layout branch forgot insets" bug).
    auto buildRasterSnapshot = [&]() {
        FramePresenter::Snapshot s;
        s.vpWidth     = viewportWidth_;
        s.vpHeight    = viewportHeight_;
        s.insetTop    = contentTop();
        s.insetRight  = contentRight();
        s.insetBottom = contentBottom();
        s.scrollY     = scrollY_;
        return s;
    };

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        double frameStart = util::currentTimeMs();

        // Advance the bro.time scaled clock (pause + timescale). Everything
        // gameplay-visible reads engineNowMs_ this frame: JS timers, rAF
        // timestamps, performance.now, CSS transitions/animations (via the
        // layout snapshot), the physics accumulator, scene agents/animations,
        // and iframe sub-documents. Engine chrome (system panels, GC cadence,
        // UI throttle, frame pacing) stays on wall time so pause never
        // freezes the shell. Deliberately unclamped: a long stall while
        // unpaused jumps the clock exactly like the pre-scaled-clock timers.
        double wallFrameDtMs = 0.0;
        if (lastWallTickMs_ > 0.0 && frameStart > lastWallTickMs_)
            wallFrameDtMs = frameStart - lastWallTickMs_;
        lastWallTickMs_ = frameStart;
        engineNowMs_ += wallFrameDtMs * effectiveTimeScale();

        // 0. Drain any in-flight layout result so event handlers don't race
        //    layout thread reads. JS handlers can mutate the DOM.
        if (layoutPipeline_->waitForIdle()) {
            if (document_ && document_->documentElement()) {
                auto& box = document_->documentElement()->layoutBox();
                documentHeight_ = box.marginBox().height;
            }
            // Drain animation events that the layout thread queued.
            for (auto& ev : transitionManager_.takePendingEvents()) {
                dom::TransitionEvent tevt(ev.type, true, false);
                tevt.setPropertyName(ev.name);
                tevt.setElapsedTime(ev.elapsedTime);
                tevt.setIsTrusted(true);
                dispatchEvent(ev.element, tevt);
            }
            for (auto& ev : animationManager_.takePendingEvents()) {
                dom::AnimationEvent aevt(ev.type, true, false);
                aevt.setAnimationName(ev.name);
                aevt.setElapsedTime(ev.elapsedTime);
                aevt.setIsTrusted(true);
                dispatchEvent(ev.element, aevt);
            }
            // element.animate() finishes → finished promise + onfinish.
            if (jsRuntime_) {
                auto fin = webAnimationManager_.takeFinishedEvents();
                if (!fin.empty())
                    js::deliverWebAnimationFinishEvents(jsRuntime_->getContext(),
                                                        std::move(fin));
            }
        }

        // 0a2. Drain a queued top-level location.reload(). This is the one
        //      safe point windowed: no JS is on the stack (the doomed realm
        //      requested the reload from a handler LAST frame), layout was
        //      just waited idle above, and we additionally require the raster
        //      worker idle — the teardown frees canvas scenes / iframe state
        //      the worker replays. If raster is still busy this frame, the
        //      request simply waits (uiDirty_ stays set via requestAppReload,
        //      so the loop keeps coming back here).
        if (pendingAppReload_ && framePresenter_->isRasterIdle()) {
            processPendingAppReload();
        }

        // Destroy DOM nodes queued for deferred free. The raster thread no
        // longer reads the DOM (it replays a CommandBuffer that holds no
        // dom::Node pointers — variable-length payloads are copied into the
        // arena, and Cmd_LayerBreak/Cmd_BlitCanvasInline carry CanvasScene*
        // which is owned by the engine, not by Elements). Layout is idle by
        // the wait above. The remaining gate is structure-clean — the
        // persistent layout tree's LayoutNodeAdapters point at live Nodes,
        // and hitTestText (called below during event polling) would return
        // a dangling TextNode* if we freed before the next layout pass
        // refreshed the tree.
        if (document_ && !document_->isStructureDirty()) {
            document_->drainPendingFrees();
            // Input-layer references to freed nodes need no scrubbing here:
            // hover/drag/click targets are dom::NodeHandles that resolve to
            // null once their node is destroyed.
        }

        // Pump HTMLMediaElement events on every <video> — must happen on the
        // main thread since QuickJS isn't thread-safe.
        pumpVideoEvents();

        // 0b. Claim the previous frame's raster result if the worker has
        //     published one. The worker never reads the DOM (it only replays
        //     the command buffer recorded last frame on this thread), so JS
        //     can mutate freely from here on without racing paint. No wait
        //     needed; consumeIfReady is non-blocking. If the worker is still
        //     busy (rare — only when raster outpaces a slow main thread),
        //     `layers` stays as the last claimed view and we composite that.
        framePresenter_->consumeIfReady();
        auto layers = framePresenter_->currentLayers();

        // 0c. Drain detached canvas scenes deferred from a previous frame.
        //     Layers/commands name scenes by sceneId and these were
        //     unregistered at detach, so no buffer scrubbing is needed — but
        //     destruction still waits for the raster worker to go idle:
        //     inline-blit commands replayed on the worker hold raw pointers.
        //     If the worker is still busy this frame the scenes simply wait.
        if (!canvasScenesDetached_.empty() && framePresenter_->isRasterIdle()) {
            for (auto& cs : canvasScenesDetached_) {
                // Release GPU resources on the worker that owns them before the
                // unique_ptr dtor runs on this (main) thread.
                if (cs->isThreaded() && canvasRasterThread_)
                    canvasRasterThread_->releaseScene(cs.get());
            }
            canvasScenesDetached_.clear();
        }

        // 1. Poll platform events
        eventLoop_->pollEvents();
        if (eventLoop_->shouldQuit()) {
            running_ = false;
            break;
        }

        // 1b. Consume physics step from previous frame.
#if BRO_WITH_PHYSICS
        if (physicsWorld_) {
            physicsWorld_->consumeStep();
        }
#endif

        // 1c. Prune detached scene graphs / WebGL contexts (elements removed
        //     from DOM). Without this, webglEntries_[0] below can refer to a
        //     stale canvas whose layout box drives a spurious resize() of the
        //     wrong FBO.
        auto isDetached = [](dom::Element* el) {
            if (!el) return false;
            auto* n = el;
            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
            return n->tagName() != "html" && n->tagName() != "HTML";
        };
#if BRO_WITH_3D
        sceneGraphs_.erase(
            std::remove_if(sceneGraphs_.begin(), sceneGraphs_.end(),
                [&](auto& sg) { return isDetached(sg.element); }),
            sceneGraphs_.end());
#endif
        webglEntries_.erase(
            std::remove_if(webglEntries_.begin(), webglEntries_.end(),
                [&](auto& e) { return isDetached(e.element); }),
            webglEntries_.end());

        // Sync scene graph physics (body transforms → node transforms).
#if BRO_WITH_3D
#if BRO_WITH_PHYSICS
        // Render alpha for transform interpolation: the fraction of a fixed
        // step accumulated so far (the accumulator itself is only advanced at
        // the step signal below, so add the wall time elapsed since then).
        if (physicsWorld_ && physicsWorld_->interpolation()) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            double pendingMs = physicsAccumMs_;
            if (lastPhysicsTimeMs_ > 0.0)
                pendingMs += (util::currentTimeMs() - lastPhysicsTimeMs_) *
                             effectiveTimeScale();
            physicsWorld_->setRenderAlpha(
                stepMs > 0.0 ? static_cast<float>(pendingMs / stepMs) : 1.0f);
        }
#endif
        for (auto& sg : sceneGraphs_) sg.graph->syncPhysics();
#endif

        // 1d. Sync scene graph AI bindings (world.tick, per-agent think).
        {
            // Scaled-clock dt — agents and scene animations obey bro.time
            // (frozen while paused, stretched by timescale).
            double nowMs = engineNowMs_;
            float frameDt = (lastFrameTimeMs_ > 0.0)
                ? static_cast<float>((nowMs - lastFrameTimeMs_) / 1000.0)
                : 1.0f / 60.0f;
            if (frameDt < 0.0f) frameDt = 0.0f;
            if (frameDt > 0.1f) frameDt = 0.1f;
            lastFrameTimeMs_ = nowMs;
            // Advance dynamic navmesh-obstacle tile rebuilds before agents
            // sync, so a batch that finishes this frame repaths them this
            // frame. (Detour ignores dt; rebuilds progress even while
            // bro.time is paused — pending edits shouldn't wedge on pause.)
            js::pumpNavMeshObstacles(frameDt);
#if BRO_WITH_3D
            for (auto& sg : sceneGraphs_) {
                sg.graph->syncAgents(frameDt);
                sg.graph->tickAnimations(frameDt);
            }
            // Scene-attached audio emitters + camera listener: push node
            // world positions (and dt-derived velocities → Doppler) into
            // broaudio AFTER animations/tweens moved nodes this frame.
            js::syncAudioSceneEmitters(frameDt);
#endif
            // Wheel smoothing is UI feel, not gameplay — wall dt, so
            // scrolling still eases out while the game is paused.
            float wheelDt = wallFrameDtMs > 0.0
                ? static_cast<float>(wallFrameDtMs / 1000.0)
                : 1.0f / 60.0f;
            if (wheelDt > 0.1f) wheelDt = 0.1f;
            drainWheelSmoothing(wheelDt);
        }

        // 2. Tick timers + JS execution. `now` is WALL time for engine
        //    chrome (system panels, GC, UI throttle); the JS-visible clocks
        //    (timers, rAF, iframes) tick with the scaled engineNowMs_.
        double now = util::currentTimeMs();
        double t0 = now;
        timers_->tick(engineNowMs_);

        auto pumpBrokit = [&](const char* fnName) {
            JSContext* ctx = jsRuntime_->getContext();
            JSValue global = JS_GetGlobalObject(ctx);
            JSValue tickFn = JS_GetPropertyStr(ctx, global, fnName);
            if (JS_IsFunction(ctx, tickFn)) {
                JSValue ret = JS_Call(ctx, tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(ctx, ret);
            }
            JS_FreeValue(ctx, tickFn);
            JS_FreeValue(ctx, global);
        };
        pumpBrokit("__brokit_fetch_tick");
        pumpBrokit("__brokit_ws_tick");
        pumpBrokit("__brokit_fs_watch_tick");

        // 2c. Tick system panel timers
        tickSystemPanels(now);
        // Tick iframe sub-document timers + requestAnimationFrame. Fold any
        // sub-doc activity (reload, DOM change, animation) into uiDirty_ like
        // systemDirty_ below — it's the only route iframe rendering has to the
        // raster thread; without it the preview never records (blank / null
        // capture). Iframes host app content, so they run on the scaled
        // clock and freeze entirely while paused (their rAF included).
        if (!timePaused_ && tickIframes(engineNowMs_)) uiDirty_ = true;
        // System panels (splash animation, menu, perf, settings) share the
        // raster thread, signaled via uiDirty_. Their own DOM edits never
        // touch the app document, so propagate systemDirty_ so the raster
        // thread actually gets kicked each frame the splash animates.
        if (systemDirty_) uiDirty_ = true;

        // 3. Bind WebGL FBO before JS callbacks (gl.bindFramebuffer(null) →
        //    canvas). Resize WebGL FBO to match element layout if needed.
        webgl::WebGL2RenderingContext* activeWebGL = nullptr;
        if (!webglEntries_.empty()) {
            auto& entry = webglEntries_[0];
            activeWebGL = entry.context.get();
            if (entry.element) {
                auto& box = entry.element->layoutBox();
                int ew = static_cast<int>(box.contentRect.width);
                int eh = static_cast<int>(box.contentRect.height);
                if (ew > 0 && eh > 0 &&
                    (ew != activeWebGL->canvasWidth() || eh != activeWebGL->canvasHeight())) {
                    activeWebGL->resize(ew, eh);
                }
            }
            activeWebGL->bindCanvasFBO();
        }

        // 3a. Fire requestAnimationFrame callbacks. rAF is the web's
        //     per-frame gameplay hook (_process analog), so pause skips it
        //     entirely — the engine keeps compositing, visuals just freeze.
        //     Timescale does NOT change the firing cadence, only the
        //     timestamp the callback receives.
        if (!timePaused_) timers_->fireAnimationFrames(engineNowMs_);

        double tGlSave = util::currentTimeMs();

        // 3b. Run pending JS jobs (promises, etc.)
        jsRuntime_->executePendingJobs();
        js::tickWorkers(jsRuntime_->getContext());
        // Deliver any results (wake/kws/gesture fires, mic chunks) the audio
        // subsystems published since last frame on this (main) thread. Each pump
        // is registered at install time only when its subsystem is built — the
        // worker drains the mic rings on its own clock, not from here.
        for (auto& pump : framePumps_) pump();
        js::tickAsync(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();
        if (steamService_) {
            js::SteamBindings::poll(jsRuntime_->getContext());
            jsRuntime_->executePendingJobs();
        }

        if (activeWebGL) activeWebGL->unbindCanvasFBO();

        // 3c1. Materialize dirty HtmlNodes on the main thread (layout + paint
        //      + GL upload). Runs here — not on the raster thread — so JS
        //      mutations to each HtmlNode's detached Document stay on the
        //      same thread that reads it during style resolution + layout.
#if BRO_WITH_3D
        if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) sg.graph->materializeHtmlNodes(skia);
            }
        }

        // 3c2. Auto-render scene graphs (after JS has updated camera/transforms).
        //      Resize to match element layout if needed (mirrors WebGL above).
        for (auto& sg : sceneGraphs_) {
            if (sg.element) {
                auto& box = sg.element->layoutBox();
                int ew = static_cast<int>(box.contentRect.width);
                int eh = static_cast<int>(box.contentRect.height);
                if (ew > 0 && eh > 0 &&
                    (ew != sg.graph->canvasWidth() || eh != sg.graph->canvasHeight())) {
                    sg.graph->setCanvasSize(ew, eh);
                }
            }
            sg.graph->render();
        }
#endif  // BRO_WITH_3D

        double tJs = util::currentTimeMs();
        accumJsMs_      += tJs - t0;
        accumGlStateMs_ += tJs - tGlSave;  // GL save is inside JS phase

        // 3d. Periodic QuickJS cycle-collector GC + orphan sweep
        if (now - lastGCMs_ >= kGCIntervalMs) {
            js::DomBindings::sweepOrphanedWrappers(jsRuntime_->getContext());
            JS_RunGC(jsRuntime_->getRuntime());
            lastGCMs_ = now;
        }

        // 3e. Signal physics thread at fixed rate (not every frame).
#if BRO_WITH_PHYSICS
        if (physicsWorld_ && physicsWorld_->isIdle()) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            double nowPhys = util::currentTimeMs();
            if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
            // Wall delta scaled by bro.time — the fixed timestep is preserved;
            // pause/timescale change how much sim time accumulates per wall
            // second (paused ⇒ 0 ⇒ no steps; scale 2 ⇒ double sim speed).
            physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_) * effectiveTimeScale();
            lastPhysicsTimeMs_ = nowPhys;
            if (physicsAccumMs_ >= stepMs) {
                physicsAccumMs_ -= stepMs;
                if (physicsAccumMs_ > stepMs * 3) physicsAccumMs_ = 0;
                physicsWorld_->signalStep();
            }
        }
#endif

        // 4. Signal layout thread when DOM is dirty. Layout is awaited just
        //    below (step 5a) so the raster signal at 5a2 sees a fully resolved
        //    tree. Only signal when both layout + raster are idle so reads
        //    don't overlap with JS mutations.
        double tLayout = tJs;
        bool layoutIdle = layoutPipeline_->isIdle();
        bool layoutSignaled = false;
        bool animActive = layoutPipeline_->animationsActive();

        // Scene-graph HtmlNodes own detached DOM trees the main pipeline
        // doesn't otherwise see; force a pass when any is dirty so imperative
        // JS edits via node.root actually re-rasterize.
        bool sceneHtmlDirty = false;
#if BRO_WITH_3D
        for (auto& sg : sceneGraphs_) {
            if (sg.graph && sg.graph->hasPendingHtmlWork()) { sceneHtmlDirty = true; break; }
        }
#endif

        // Was the document dirtied by a real base change (DOM/style/hover/…)?
        // Promoted transform/opacity animations no longer markDirty (the layout
        // tick routes them to promotedElements_ instead), so this is false on a
        // promoted-only frame — which is exactly what lets the base cache be
        // reused there. Captured before layout clears the flag.
        bool baseWasDirty = document_ && document_->isDirty();

        if (layoutIdle && framePresenter_->isRasterIdle() && document_ &&
            (baseWasDirty || animActive || sceneHtmlDirty || !hasRenderedOnce_)) {
            if (document_->isStructureDirty()) {
                ensureReplacedElements(document_->documentElement());
                // The mutation may have added or removed an <iframe>. Only note
                // it here — the layout pass below clears structureDirty_, and
                // sub-docs may only be created/destroyed at the raster-idle
                // point (see the consume site next to recordIframeLayers).
                iframeSyncNeeded_ = true;
            }
            LayoutPipeline::Snapshot ls;
            ls.vpWidth         = viewportWidth_;
            ls.vpHeight        = viewportHeight_;
            ls.insetTop        = contentTop();
            ls.insetRight      = contentRight();
            ls.insetBottom     = contentBottom();
            ls.animationsActive = animActive;
            ls.hoveredElement  = hoveredElement_.get();
            ls.timeMs          = engineNowMs_;  // CSS transitions obey bro.time
            layoutPipeline_->signalLayout(ls);
            layoutSignaled = true;
        }
        accumLayoutMs_ += util::currentTimeMs() - tJs;

        // === GPU FRAME (threaded rasterization + main-thread compositing) ===
        // Raster thread runs in parallel with composite + swap below
        // (signaled at step 5a2 once layout has been claimed).

        double tRaster = util::currentTimeMs();

        // 5a. Wait for the layout thread and consume results.
        //
        //     Layout is on its own thread but is NOT, today, overlapped with
        //     anything: step 4 signals it only after the JS phase and the
        //     scene/canvas updates have already finished, and we block on it
        //     here a few lines later. In wall-clock terms it is sequential —
        //     the thread buys thread-affinity for the layout data, not
        //     concurrency. Don't read the handoff as a parallelism win.
        //
        //     It has to stay this way as long as raster is signaled from the
        //     resolved tree: layout and any post-layout JS must complete
        //     *before* 5a2 so the raster worker reads a fully resolved tree.
        //     (Trade-off: layout does not overlap composite + swap; raster
        //     does. Raster is the heavier of the two and benefits more from
        //     the vsync window.)
        //
        //     Because the wait is sequential, the layout thread's whole cost —
        //     style resolution included — lands here, inside the raster phase's
        //     clock. Bill it to Layout instead and subtract it from Raster
        //     below: a HUD that reports "Layout 0.01 ms, Raster 25 ms" while a
        //     style pass eats the frame sends you hunting through the
        //     rasterizer for a bug that is in the cascade.
        double layoutWaitMs = 0.0;
        if (layoutSignaled) {
            double tWait = util::currentTimeMs();
            bool layoutClaimed = layoutPipeline_->waitClaimDone();
            layoutWaitMs = util::currentTimeMs() - tWait;
            if (layoutClaimed) {
                if (document_ && document_->documentElement()) {
                    auto& box = document_->documentElement()->layoutBox();
                    documentHeight_ = box.marginBox().height;
                }

                // Process auto-scroll-to-bottom for tracked overflow elements.
                if (document_ && !document_->scrollToBottomElements().empty()) {
                    auto pending = document_->scrollToBottomElements();
                    for (auto* elem : pending) {
                        std::string ov = getOverflowY(elem->computedStyle());
                        if (overflowClips(ov)) {
                            elem->setScrollTopValue(maxScrollTop(elem));
                        }
                        elem->setScrollToBottom(false);
                    }
                }

                // Notify ResizeObserver / IntersectionObserver after layout.
                if (jsRuntime_) {
                    auto* ctx = jsRuntime_->getContext();
                    if (JS_IsUndefined(observerCheckFn_)) {
                        observerCheckFn_ = JS_Eval(ctx, js_observer_check,
                                                   strlen(js_observer_check),
                                                   "<observer-check>",
                                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
                    }
                    if (!JS_IsUndefined(observerCheckFn_) && !JS_IsException(observerCheckFn_)) {
                        JSValue r = JS_EvalFunction(ctx, JS_DupValue(ctx, observerCheckFn_));
                        JS_FreeValue(ctx, r);
                    }
                }

                // Drain CSS transition/animation events on the main thread.
                for (auto& ev : transitionManager_.takePendingEvents()) {
                    dom::TransitionEvent tevt(ev.type, true, false);
                    tevt.setPropertyName(ev.name);
                    tevt.setElapsedTime(ev.elapsedTime);
                    tevt.setIsTrusted(true);
                    dispatchEvent(ev.element, tevt);
                }
                for (auto& ev : animationManager_.takePendingEvents()) {
                    dom::AnimationEvent aevt(ev.type, true, false);
                    aevt.setAnimationName(ev.name);
                    aevt.setElapsedTime(ev.elapsedTime);
                    aevt.setIsTrusted(true);
                    dispatchEvent(ev.element, aevt);
                }
                // element.animate() finishes → finished promise + onfinish.
                if (jsRuntime_) {
                    auto fin = webAnimationManager_.takeFinishedEvents();
                    if (!fin.empty())
                        js::deliverWebAnimationFinishEvents(
                            jsRuntime_->getContext(), std::move(fin));
                }

                // matchMedia change events — the layout pass above resolved
                // styles against any new media context, so listeners observe
                // a consistent world. Per-realm generation gate makes this a
                // cheap no-op when nothing media-relevant happened.
                deliverMediaQueryChangesAllRealms();

                // Flush microtasks from event handlers (may have queued DOM mutations).
                jsRuntime_->executePendingJobs();

                // Re-record the base only when something base-affecting changed.
                // A promoted-only frame (just a transform/opacity animation
                // advancing) leaves the cached base untouched; its promoted
                // layer is re-recorded separately in step 5a2. A change in the
                // promoted set (an element starting/finishing its animation)
                // moves the base's skip-holes, so it also forces a base rebuild.
                bool promotedSetChanged = (promotedElements_ != basePromotedSet_);
                if (baseWasDirty || promotedSetChanged || !baseValid_) {
                    uiDirty_ = true;       // a frame must be recorded
                    appBaseDirty_ = true;  // and the cached base rebuilt
                }
            }
        }

        // 5a2. Record draw commands into the back command buffer, then signal
        //      raster with a fully-populated snapshot. The raster thread will
        //      replay the buffer against its own Skia/GL context — it never
        //      reads the DOM, so there is no race with the next frame's JS.
        //      Composite + swap below run in parallel with the worker.
        if (framePresenter_->isRasterIdle()) {
            bool uiThrottled = (now - lastUIRenderMs_ < uiFrameIntervalMs_);
            bool promotedActive = layoutPipeline_->promotedActive();
            // Record a frame whenever anything needs a redraw: app base
            // (uiDirty_, via document/input), a promoted animation advancing, or
            // system panels / overlays (also uiDirty_). Whether the *base* is
            // rebuilt within is a separate, narrower decision below.
            if ((uiDirty_ || !hasRenderedOnce_ || promotedActive) && !uiThrottled) {
                stageSystemPanelCanvases();
                updateSelectionSnapshot();

                // Record into the same slot the worker will pick up — the
                // state machine release/acquire ordering on signalRender
                // makes these writes visible before the worker's read.
                auto& backBuf = framePresenter_->backBuffer();
                auto rsnap = buildRasterSnapshot();

                // promotedElements_ was published by the layout pass claimed
                // above; the barrier makes it visible here. Non-empty ⇒ split
                // the record into a cached base (holes) + an on-top promoted
                // layer; empty ⇒ the classic single full-pass record.
                const std::unordered_set<dom::Element*>* pset =
                    promotedElements_.empty() ? nullptr : &promotedElements_;

                // Rebuild the cached base ONLY on a genuine app-base change.
                // appBaseDirty_ (set in the claim above) covers document/style/
                // hover/promoted-set changes; scroll + insets are baked into the
                // recorded commands so a change in either also invalidates it.
                // Crucially this excludes the broad uiDirty_ — a splash/menu/
                // overlay animation re-records only the cheap system/promoted
                // layers, never the full DOM base.
                bool scrollOrInsetChanged =
                    rsnap.scrollY != baseScrollY_ ||
                    rsnap.insetTop != baseInsetTop_ ||
                    rsnap.insetRight != baseInsetRight_ ||
                    rsnap.insetBottom != baseInsetBottom_;
                bool baseNeedsRecord = appBaseDirty_ || scrollOrInsetChanged ||
                                       !baseValid_ || !hasRenderedOnce_;

                if (baseNeedsRecord) {
                    // Rebuild + cache the base (skipping promoted subtrees when
                    // pset != null; a full pass when null). Reused verbatim on
                    // promoted-only frames.
                    recordAppLayers(baseCommands_,
                                    rsnap.vpWidth, rsnap.vpHeight,
                                    rsnap.insetTop, rsnap.insetRight, rsnap.insetBottom,
                                    rsnap.scrollY, pset, /*promotedOnly=*/false);
                    basePromotedSet_ = promotedElements_;
                    baseScrollY_ = rsnap.scrollY;
                    baseInsetTop_ = rsnap.insetTop;
                    baseInsetRight_ = rsnap.insetRight;
                    baseInsetBottom_ = rsnap.insetBottom;
                    baseValid_ = true;
                    appBaseDirty_ = false;
                }
                // Re-record the (small) promoted layer every frame so its
                // transform is current; clear it when nothing is promoted.
                if (pset) {
                    recordAppLayers(backBuf.promotedCommands,
                                    rsnap.vpWidth, rsnap.vpHeight,
                                    rsnap.insetTop, rsnap.insetRight, rsnap.insetBottom,
                                    rsnap.scrollY, pset, /*promotedOnly=*/true);
                } else {
                    backBuf.promotedCommands.clear();
                }

                // App layers are content-sized and content-space; stash the
                // composite placement with the frame so compositeLayers below
                // uses the insets this recording was made under.
                backBuf.appInsetTop = rsnap.insetTop;
                backBuf.appContentW = rsnap.vpWidth - rsnap.insetRight;
                backBuf.appContentH = rsnap.vpHeight - rsnap.insetTop
                                      - rsnap.insetBottom;
                recordSystemPanelLayers(backBuf.systemCommands,
                                        rsnap.vpWidth, rsnap.vpHeight);
                // Drain queued iframe.reload() requests here, where the raster
                // thread is idle — tearing a sub-doc down while it is being
                // replayed would be a use-after-free. Then record each iframe
                // sub-document into its own per-doc command buffer (main thread).
                // The raster thread replays them into box-sized surfaces; the app
                // compositor draws those textures at the UILayer::Iframe breaks
                // recorded in the app pass above. Safe to touch the per-doc
                // buffers here: this whole record block only runs when the raster
                // thread is idle (isRasterIdle above).
                // Reloads FIRST, then the add/remove sync. A JS `el.src = "app"`
                // on a fresh <iframe> does both — the src setter queues a reload
                // and the appendChild dirties the structure — so whichever of
                // these builds the sub-doc, the other must find it already there
                // and skip. Sync-first built it, then the reload tore it straight
                // back down and rebuilt it: two sub-documents, two load events.
                processPendingIframeReloads();
                // Instantiate sub-docs for <iframe>s JS just added, and tear down
                // those whose element left the tree. Same raster-idle reasoning as
                // the reload drain. The new element's layout may not have landed
                // yet, which is harmless: createIframeDoc falls back to 300x150 and
                // recordIframeLayers re-reads the real content box (and re-lays the
                // sub-doc out at it) on the very next record.
                if (iframeSyncNeeded_) {
                    syncIframes();
                    iframeSyncNeeded_ = false;
                }
                recordIframeLayers();

                framePresenter_->signalRender(rsnap);
                uiDirty_ = false;
                hasRenderedOnce_ = true;
                lastUIRenderMs_ = now;
            }
        }

        // 5b. Signal canvas threads using the now-stable layer view. Stale
        //     sceneIds (scene detached after the layer was recorded) resolve
        //     to null and are skipped.
        for (auto& layer : layers.appLayers) {
            if (layer.type != UILayer::Canvas) continue;
            if (auto* cs = canvasSceneById(layer.canvasSceneId))
                cs->prepareAndSignal();
        }

        // 5b2. Wait for canvas-thread fences before compositing the same view.
        for (auto& layer : layers.appLayers) {
            if (layer.type != UILayer::Canvas) continue;
            if (auto* cs = canvasSceneById(layer.canvasSceneId))
                cs->consumeFence();
        }
        // Record + signal + canvas fences. The layout-thread wait ran inside
        // this span but belongs to Layout (see step 5a), so take it back out.
        accumRasterMs_ += (util::currentTimeMs() - tRaster) - layoutWaitMs;
        accumLayoutMs_ += layoutWaitMs;

        double tGpu = util::currentTimeMs();

        // 5d. Update canvas scene scroll + clean up detached. The raster
        //     thread may be mid-replay holding CanvasScene* pointers from
        //     this frame's recording. Erasing here is safe because:
        //       (a) signalRender requires isRasterIdle, so the previous
        //           replay is fully complete before we publish a new one,
        //           and (b) any scene whose element was detached during this
        //           frame's JS phase was skipped by DrawTraversal at record
        //           time and so doesn't appear in the just-signaled buffer.
        //     If a future change adds JS execution between record (5a2) and
        //     this point, this invariant will need re-verification.
        for (auto& cs : canvasScenes_) {
            cs->setViewportScroll(scrollY_);
            cs->checkDetached();
        }
        for (auto& cs : canvasScenes_) {
            if (!cs->isDetached()) continue;
            // This scene is being reclaimed. If its backing Element is still
            // alive (orphaned from the DOM but not yet freed), sever the
            // Element's back-pointer so its eventual ~Element doesn't invoke
            // the on-destroy hook against this now-freed scene. backingElement()
            // is null once onElementFinalized() has run (Element freed first),
            // so this only fires in the scene-reclaimed-first ordering.
            if (auto* el = static_cast<dom::Element*>(cs->backingElement()))
                el->setCanvasScene(nullptr);
        }
        // Move detached scenes out of the active list and unregister their
        // sceneIds — from here on, any layer in either buffer that still names
        // one resolves to null at composite/signal time. Destruction is still
        // deferred to frame top with the raster worker idle (inline-blit
        // commands replayed on the worker hold raw pointers).
        for (auto it = canvasScenes_.begin(); it != canvasScenes_.end(); ) {
            if ((*it)->isDetached()) {
                canvasSceneRegistry_.erase((*it)->sceneId());
                canvasScenesDetached_.push_back(std::move(*it));
                it = canvasScenes_.erase(it);
            } else {
                ++it;
            }
        }

        // 5e. Set viewport and clear.
        glViewport(0, 0, viewportWidth_, viewportHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 5f. Composite app UI layers in DOM order. HTML layers (cached
        //     textures from the raster thread) interleaved with canvas layers
        //     (freshly rasterized on canvas threads). App layers are
        //     content-sized/content-space; place them at (0, insetTop) with
        //     the content dims recorded alongside this frame.
        compositeLayers(layers.appLayers, 0,
                        layers.appInsetTop, layers.appContentW,
                        layers.appContentH);

        // 5g. Composite system panel layers (menu bar / preferences / splash)
        //     on top of app content.
        compositeLayers(layers.systemLayers);

        // Restore WebGL shadow state so apps with internal caches (three.js)
        // see the same GL state they left on the previous frame.
        if (activeWebGL) activeWebGL->restoreState();

        // Measure GPU work before swap (swap includes vsync wait).
        accumGpuMs_ += util::currentTimeMs() - tGpu;

        // Swap buffers (may block on vsync — not counted as GPU work).
        // Raster thread runs in parallel here (signaled at step 5a2 above).
        gl_->swapBuffers();

        // 5h. Present-rate cap. vsync is our only pacing on a focused window,
        //     but Windows stops pacing wglSwapBuffers once the window loses
        //     focus — the loop then free-runs and starves the raster/layout
        //     workers, so the wall-clock-sampled CSS animations judder. Sleep
        //     to hold a minimum frame interval regardless of vsync: the app's
        //     graphics.maxFps cap, floored to kUnfocusedFps while unfocused.
        //     The more restrictive (larger interval) wins. Done before frame
        //     timing below so the perf HUD reports the true present cadence.
        {
            double capMs = frameCapIntervalMs_;  // 0 = app left it uncapped
            if (!windowFocused_)
                capMs = std::max(capMs, 1000.0 / kUnfocusedFps);
            if (capMs > 0.0) {
                double elapsed = util::currentTimeMs() - frameStart;
                double sleepMs = capMs - elapsed;
                if (sleepMs > 0.5) {
                    std::this_thread::sleep_for(std::chrono::microseconds(
                        static_cast<int64_t>(sleepMs * 1000.0)));
                }
            }
        }

        // 6. Frame time tracking.
        totalFrameMs_ = util::currentTimeMs() - frameStart;
        double totalFrameMs = totalFrameMs_;
        statsAccumMs_ += totalFrameMs;
        statsFrameCount_++;
        if (totalFrameMs < statsMinFrameMs_) statsMinFrameMs_ = totalFrameMs;
        if (totalFrameMs > statsMaxFrameMs_) statsMaxFrameMs_ = totalFrameMs;
        if (statsAccumMs_ >= 500.0) {
            statsFps_ = statsFrameCount_ / (statsAccumMs_ / 1000.0);
            statsFrameTimeMs_ = statsAccumMs_ / statsFrameCount_;
            double n = statsFrameCount_;
            phaseJsMs_      = accumJsMs_      / n;
            phaseLayoutMs_  = accumLayoutMs_  / n;
            phaseRasterMs_  = accumRasterMs_  / n;
            phaseGpuMs_     = accumGpuMs_     / n;
            phaseGlStateMs_ = accumGlStateMs_ / n;
            phaseDrawMs_    = accumDrawMs_    / n;
            phaseUploadMs_  = accumUploadMs_  / n;
            accumJsMs_ = accumLayoutMs_ = accumRasterMs_ = accumGpuMs_ = accumGlStateMs_ = 0.0;
            accumDrawMs_ = accumUploadMs_ = 0.0;
            statsAccumMs_ = 0.0;
            statsFrameCount_ = 0;
            statsMinFrameMs_ = 999.0;
            statsMaxFrameMs_ = 0.0;
            uiDirty_ = true;  // refresh overlay

            updateSystemPerf(statsFps_, statsFrameTimeMs_,
                             phaseJsMs_, phaseLayoutMs_,
                             phaseRasterMs_, phaseGpuMs_,
                             phaseDrawMs_,
                             viewportWidth_, viewportHeight_);
        }
    }

    // --- Shutdown ---
    // Quiesce worker threads and GPU contexts. ~Engine() calls this too (it is
    // idempotent), which is what gives Headless and Server — both of which
    // early-return above, never reaching this line — the same sequence.
    shutdown();
}

void Engine::removeModalEventWatch() {
    SDL_RemoveEventWatch(modalEventWatcher, this);
}

} // namespace bro::engine
