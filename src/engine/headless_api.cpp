// Engine headless/capture API methods — split from engine.cpp for readability.
// These are Engine member function implementations, not a separate class.

#include "engine/engine.h"
#include "engine/overflow.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "audio_inference/audio_inference.h"

#include "observer_check.js.h"

#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "render/command_buffer.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/ai_bindings.h"
#include "js/audio_scene_sync.h"
#include "js/dom_bindings.h"
#include "js/event_dispatch.h"
#include "js/web_animation_bindings.h"
#include "js/worker.h"
#include "js/steam_bindings.h"
#include "js/wake_bindings.h"
#include "js/gesture_bindings.h"
#include "js/kws_bindings.h"
#include "js/mic_bindings.h"
#include "js/async_job.h"
#include "js/net_bindings.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "canvas/canvas_scene.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"

#include <broaudio/engine.h>

#include "broimage/encode.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include <glad/gl.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Headless API
// ---------------------------------------------------------------------------

void Engine::flush() {
    jsRuntime_->executePendingJobs();
    // Drain Steam events into JS callbacks (onpulse and, later, friends/lobby/
    // voice). The windowed/headless main loop polls in Engine::run(), but a
    // one-shot headless script never enters run() — so flush() pumps it too,
    // letting headless steam-lab tests observe async Steam events deterministically.
    if (steamService_) {
        js::SteamBindings::poll(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();
    }
    // Pump HTMLMediaElement events from the main thread. In headless there
    // is no raster thread, but we keep the call path consistent with the
    // windowed engine so ElVideo::draw() never touches JS.
    pumpVideoEvents();
    // Tick transitions and animations, mark dirty if any are active.
    // engineNowMs_, not virtualTime_: CSS transitions/animations run on the
    // bro.time scaled clock (identical to virtual time at scale 1).
    if (document_) {
        document_->setTransitionManager(&transitionManager_, engineNowMs_);
        animationManager_.setKeyframes(&document_->cascade().keyframes());
        document_->setAnimationManager(&animationManager_);
        document_->setWebAnimationManager(&webAnimationManager_);
        bool animActive = transitionManager_.tick(engineNowMs_) |
                          animationManager_.tick(engineNowMs_) |
                          webAnimationManager_.tick(engineNowMs_);
        if (animActive) {
            // Paint-dirty, not layout-dirty: an active animation re-resolves its
            // element's style every frame regardless (resolveStylesRecursive's
            // animatingSelf), and the style diff there promotes to a layout only
            // when the animation actually moves geometry. A transform-only
            // spinner would otherwise drag the whole document through
            // layoutTree() on every frame it turns.
            document_->markPaintDirty();
        }
    }
    // Re-layout any dirty system panel docs so subsequent hit-tests see the
    // current DOM state (click() fires events which can mutate system panel
    // DOM — e.g. bro.menu dropdowns — and we need layout fresh before the
    // next click tests against it).
    if (textMetrics_) {
        bool anyDirty = false;
        for (auto& sdoc : systemDocs_) {
            if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;
            if (sdoc.document->isDirty()) { anyDirty = true; break; }
        }
        if (anyDirty) {
            layoutSystemPanels(*textMetrics_);
            systemDirty_ = true;
        }
    }

    // Latched before the layout pass below clears it: the iframe sync after this
    // block needs to know an element was added or removed, but must run AFTER
    // layout so createIframeDoc sees a real content box.
    bool structureChanged = false;
    if (document_ && document_->isDirty()) {
        structureChanged = document_->isStructureDirty();
        if (structureChanged) {
            ensureReplacedElements(document_->documentElement());
        }
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_.get());
        document_->resolveStyles();
        // Skip the full layoutTree() pass when only a paint-only change (a hover
        // restyle) is pending and resolveStyles() found no geometry change —
        // mirrors the windowed layout thread. performLayout() rebuilds the
        // persistent layout tree when structureDirty_ is set and clears the flag.
        if (document_->isLayoutDirty() || document_->isStructureDirty() ||
            !document_->layoutRoot()) {
            document_->performLayout(static_cast<float>(viewportWidth_),
                                     static_cast<float>(contentHeight()), *textMetrics_);
        }
        document_->clearDirty();

        // Keep the scrollable extent in sync with the fresh layout, same as
        // the windowed loop does after every layout drain. Without this,
        // headless documentHeight_ froze at its boot value, so viewport
        // wheel scrolling clamped to a stale (usually ~0) maxScroll and the
        // viewport scrollbar drew against boot-time geometry.
        if (auto* rootEl = document_->documentElement()) {
            documentHeight_ = rootEl->layoutBox().marginBox().height;
        }

        // Apply any deferred scroll-to-bottom now that layout is fresh (mirrors
        // the windowed loop). A JS `el.scrollTop = el.scrollHeight` issued in the
        // same turn as an append registered the element here because the append
        // wasn't laid out yet; snap it to the true bottom now.
        if (!document_->scrollToBottomElements().empty()) {
            auto pending = document_->scrollToBottomElements();
            for (auto* elem : pending) {
                if (overflowClips(getOverflowY(elem->computedStyle())))
                    elem->setScrollTopValue(maxScrollTop(elem));
                elem->setScrollToBottom(false);
            }
        }

        // Notify ResizeObserver / IntersectionObserver after layout
        if (jsRuntime_) {
            JSValue r = JS_Eval(jsRuntime_->getContext(), js_observer_check,
                                strlen(js_observer_check), "<observer-check>", JS_EVAL_TYPE_GLOBAL);
            JS_FreeValue(jsRuntime_->getContext(), r);
        }

    }

    // The headless counterpart of the iframe block in the windowed frame loop,
    // in the same order and for the same reason: a JS `el.src = "app"` on a fresh
    // <iframe> queues a reload AND dirties the structure, so whichever of these
    // builds the sub-doc, the other must find it already there and skip.
    //
    // Windowed drains reloads in the frame loop and captureIframe() drains them
    // too, but a headless script that reloads and then screenshots (rather than
    // capturing) would otherwise never see the new sub-document. Layout has run
    // by here, so createIframeDoc sees each element's real content box, and with
    // no raster thread there is nothing to quiesce first.
    processPendingIframeReloads();
    if (structureChanged) syncIframes();
    // Secondary-window drain — the headless counterpart of the frame loop's
    // raster-idle drain (there is no raster thread here). This is what makes
    // open()/close() assertable from a test: open → flush() materializes the
    // hidden window; close → flush() destroys it and fires 'close'.
    processPendingWindowHosts();
    // Catch each host up with its window's current client size here, at the
    // drain, rather than leaving it to whenever capture() next runs: the size
    // change fires a 'resize' event in BOTH realms, and app JS belongs at the
    // drain point, not reentrantly inside a capture() call. Idempotent - a
    // host whose box already matches returns immediately.
    for (auto& h : windowHosts_) syncWindowHostBox(*h);
    // ...and its message traffic, so a test can post and assert after one flush().
    drainWindowHostMessages();

    // Drain CSS transition/animation events and dispatch to JS.
    {
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

        // matchMedia change events — resolveStyles above consumed any pending
        // media-context change for the app document; iframe/system-panel
        // realms deliver once their own restyle lands (per-realm gate).
        deliverMediaQueryChangesAllRealms();
    }

    // Sync each scene graph's canvas dimensions from the element's layout
    // box, then render. Mirrors the windowed main loop so flush()-driven
    // capture paths (screenshotCanvas, scene.toImageData) see fresh content
    // without having to advanceTime by a step. Without this, JS code that
    // creates a scene canvas + populates it + immediately reads pixels would
    // see an unallocated FBO because render() short-circuits when canvas
    // size is zero.
#if BRO_WITH_3D
    if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        for (auto& sg : sceneGraphs_) {
            if (sg.graph) sg.graph->materializeHtmlNodes(skia);
        }
    }
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
        if (sg.graph) sg.graph->render();
    }

    // Prune detached scene graphs (elements removed from DOM).
    // Must happen before canvas scene pruning so the scene graph releases
    // its canvasScene_ pointer before the CanvasScene is destroyed.
    sceneGraphs_.erase(
        std::remove_if(sceneGraphs_.begin(), sceneGraphs_.end(),
            [](auto& sg) {
                if (!sg.element) return false;
                auto* n = sg.element;
                while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                return n->tagName() != "html" && n->tagName() != "HTML";
            }),
        sceneGraphs_.end());
#endif  // BRO_WITH_3D

    // Prune detached WebGL contexts (canvas elements removed from DOM).
    webglEntries_.erase(
        std::remove_if(webglEntries_.begin(), webglEntries_.end(),
            [](auto& entry) {
                if (!entry.element) return false;
                auto* n = entry.element;
                while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                return n->tagName() != "html" && n->tagName() != "HTML";
            }),
        webglEntries_.end());

    // Prune detached canvas scenes (elements removed from DOM)
    for (auto& cs : canvasScenes_) {
        cs->rasterize(gl_.get());  // triggers detached check
        if (cs->isDetached()) canvasSceneRegistry_.erase(cs->sceneId());
    }
    canvasScenes_.erase(
        std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
            [](auto& cs) { return cs->isDetached(); }),
        canvasScenes_.end());
}

void Engine::advanceTime(double ms) {
    if (displayMode_ != DisplayMode::Headless) return;

    // Get active WebGL context for FBO binding
    webgl::WebGL2RenderingContext* activeWebGL = nullptr;
    if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();

    double remaining = ms;
    while (remaining > 0) {
        double step = std::min(remaining, 16.0);
        virtualTime_ += step;
        remaining -= step;

        // bro.time composition: virtualTime_ is the headless wall-clock
        // analog (system panels, splash, GC, audio, brokit pumps advance by
        // the full step); the scaled clock advances by step * effective
        // scale — nothing at all while paused. Only ONE scaling happens
        // here; every scaled consumer below reads engineNowMs_/scaledStep
        // directly, so pause/timescale can never double-apply.
        double scaledStep = step * effectiveTimeScale();
        engineNowMs_ += scaledStep;

        // Drain microtasks before firing any macrotask (timer callbacks).
        // Without this, microtasks queued by the calling synchronous script
        // (queueMicrotask, Promise.resolve().then) would run AFTER a 0-delay
        // setTimeout, violating the HTML microtask-checkpoint ordering.
        jsRuntime_->executePendingJobs();

        // Apply pending viewport wheel scroll, same as the windowed frame
        // loop (engine_frame.cpp). Without this, headless wheel() over the
        // viewport parked pixels in wheelResidualY_ forever and the document
        // never scrolled — a windowed/headless divergence of exactly the
        // kind that hid the menu-inset layer bug.
        drainWheelSmoothing(static_cast<float>(step) / 1000.0f);

        timers_->tick(engineNowMs_);

        // Tick brokit fetch (pump pending HTTP requests)
        {
            JSValue global = JS_GetGlobalObject(jsRuntime_->getContext());
            JSValue tickFn = JS_GetPropertyStr(jsRuntime_->getContext(), global, "__brokit_fetch_tick");
            if (JS_IsFunction(jsRuntime_->getContext(), tickFn)) {
                JSValue ret = JS_Call(jsRuntime_->getContext(), tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(jsRuntime_->getContext(), ret);
            }
            JS_FreeValue(jsRuntime_->getContext(), tickFn);
            JS_FreeValue(jsRuntime_->getContext(), global);
        }

        // Tick brokit WebSocket (pump pending connections/messages)
        {
            JSValue global = JS_GetGlobalObject(jsRuntime_->getContext());
            JSValue tickFn = JS_GetPropertyStr(jsRuntime_->getContext(), global, "__brokit_ws_tick");
            if (JS_IsFunction(jsRuntime_->getContext(), tickFn)) {
                JSValue ret = JS_Call(jsRuntime_->getContext(), tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(jsRuntime_->getContext(), ret);
            }
            JS_FreeValue(jsRuntime_->getContext(), tickFn);
            JS_FreeValue(jsRuntime_->getContext(), global);
        }

        // Tick brokit net (raw TCP/UDP sockets + WebSocketServer)
        {
            JSValue global = JS_GetGlobalObject(jsRuntime_->getContext());
            JSValue tickFn = JS_GetPropertyStr(jsRuntime_->getContext(), global, "__brokit_net_tick");
            if (JS_IsFunction(jsRuntime_->getContext(), tickFn)) {
                JSValue ret = JS_Call(jsRuntime_->getContext(), tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(jsRuntime_->getContext(), ret);
            }
            JS_FreeValue(jsRuntime_->getContext(), tickFn);
            JS_FreeValue(jsRuntime_->getContext(), global);
        }

        // Tick brokit fs.watch (deliver native filesystem events to JS)
        {
            JSValue global = JS_GetGlobalObject(jsRuntime_->getContext());
            JSValue tickFn = JS_GetPropertyStr(jsRuntime_->getContext(), global, "__brokit_fs_watch_tick");
            if (JS_IsFunction(jsRuntime_->getContext(), tickFn)) {
                JSValue ret = JS_Call(jsRuntime_->getContext(), tickFn, JS_UNDEFINED, 0, nullptr);
                JS_FreeValue(jsRuntime_->getContext(), ret);
            }
            JS_FreeValue(jsRuntime_->getContext(), tickFn);
            JS_FreeValue(jsRuntime_->getContext(), global);
        }

        if (activeWebGL) activeWebGL->bindCanvasFBO();
        // rAF skips entirely while paused (the web's _process analog);
        // timescale changes only the timestamp, not the firing cadence.
        if (!timePaused_) timers_->fireAnimationFrames(engineNowMs_);
        jsRuntime_->executePendingJobs();
        js::tickWorkers(jsRuntime_->getContext());
        // Run audio-inference models inline on this thread (headless has no
        // worker thread — the parity convention, cf. physicsWorld_->stepInline)
        // so wake fires are produced deterministically before we deliver them.
        if (audioInference_) audioInference_->stepInline();
        // Deliver audio results (registered pumps: wake/kws/gesture, mic).
        for (auto& pump : framePumps_) pump();
        bro::js::tickAsync(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();

        // Tick system panels so splash lifecycle (min-display + dismiss)
        // advances with virtual time.
        tickSystemPanels(virtualTime_);

        // Same for <iframe> sub-documents: each owns its own Timers, so without
        // this an embedded app's setTimeout/setInterval/rAF never fire under
        // headless virtual time — the windowed loop ticks them every frame
        // (engine_frame.cpp), headless ticked nothing. Iframes host app
        // content, so they run on the scaled clock and freeze while paused.
        if (!timePaused_) tickIframes(engineNowMs_);
        // Secondary-window documents own their Timers too — without this an
        // app opened via bro.window.open() would never see setTimeout/rAF fire
        // under headless virtual time.
        if (!timePaused_) tickWindowHosts(engineNowMs_);

        // Network polling is delivered via a frame pump (registered in
        // engine_init when BRO_WITH_NET is on) — see the framePumps_ loop above.

        // Poll Steam too (parity with net) so virtual-time advanceTime/sleep
        // drains friends/lobby/avatar/voice events, not just flush(). The pulse
        // heartbeat is still real-time (it's the service thread's cadence), but
        // async results posted by the service show up under advanceTime now.
        if (steamService_) {
            js::SteamBindings::poll(jsRuntime_->getContext());
            jsRuntime_->executePendingJobs();
        }

        if (activeWebGL) activeWebGL->unbindCanvasFBO();

        // Step physics deterministically against virtual time, synchronously
        // on the main thread (headless does not start the physics worker thread).
#if BRO_WITH_PHYSICS
        if (physicsWorld_) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            // Scaled sim-time input; fixed timestep preserved. Paused ⇒ the
            // accumulator gains nothing and the body freezes in place.
            physicsAccumMs_ += scaledStep;
            int safety = 16;
            while (physicsAccumMs_ + 0.5 >= stepMs && safety-- > 0) {
                physicsAccumMs_ -= stepMs;
                physicsWorld_->stepInline();
            }
            // Render alpha for transform interpolation: the leftover fraction
            // of a fixed step (deterministic under virtual time).
            physicsWorld_->setRenderAlpha(
                stepMs > 0.0 ? static_cast<float>(physicsAccumMs_ / stepMs) : 1.0f);
        }
#endif

        // Advance dynamic navmesh-obstacle tile rebuilds before agents sync
        // (same ordering as the windowed frame: a batch that finishes this
        // step repaths agents this step).
        js::pumpNavMeshObstacles(static_cast<float>(scaledStep * 0.001));

        // Step AI bindings once per advanceTime step (deterministic, uses the
        // scaled step as dt — agents obey bro.time like everything gameplay).
#if BRO_WITH_3D
        {
            float aiDt = static_cast<float>(scaledStep * 0.001);
            for (auto& sg : sceneGraphs_) {
                sg.graph->syncAgents(aiDt);
                sg.graph->tickAnimations(aiDt);
            }
            // Scene-attached audio emitters + camera listener — same ordering
            // as the windowed frame (after animations, before the audio
            // renderBlock below), so advanceTime-driven motion is what the
            // deterministic audio render hears.
            js::syncAudioSceneEmitters(aiDt);
        }

        // Headless has no raster thread (the normal code path relies on the
        // raster loop to materialize HtmlNodes). Run it inline on the main
        // thread using the main renderer/font manager before scene render.
        auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
        if (skia) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) {
                    sg.graph->materializeHtmlNodes(skia);
                }
            }
        }

        // Auto-render scene graphs after JS execution
        for (auto& sg : sceneGraphs_) {
            sg.graph->render();
        }
#endif  // BRO_WITH_3D

        flush();

        // Pump headless audio engine: render frames matching this time step
        if (audioEngine_) {
            int audioFrames = static_cast<int>(step * audioEngine_->sampleRate() / 1000.0 + 0.5);
            if (audioFrames > 0)
                audioEngine_->renderBlock(audioFrames);
        }

        // Periodic GC + orphan sweep (every ~1s of virtual time)
        if (virtualTime_ - lastGCMs_ >= kGCIntervalMs) {
            js::DomBindings::sweepOrphanedWrappers(jsRuntime_->getContext());
            JS_RunGC(jsRuntime_->getRuntime());
            lastGCMs_ = virtualTime_;
        }
    }
}

std::string Engine::eval(const std::string& code) {
    JSContext* ctx = jsRuntime_->getContext();
    JSValue result = JS_Eval(ctx, code.c_str(), code.size(), "<eval>",
                              JS_EVAL_TYPE_GLOBAL);
    std::string output;
    if (JS_IsException(result)) {
        js::Runtime::checkException(ctx, result);
        output = "[exception]";
    } else {
        const char* str = JS_ToCString(ctx, result);
        if (str) {
            output = str;
            JS_FreeCString(ctx, str);
        } else {
            output = "[null]";
        }
    }
    JS_FreeValue(ctx, result);
    flush();
    return output;
}

// Shared GPU readback path used by both screenshot() and capturePixels().
// Mirrors the windowed main loop: build the same UILayer list the raster
// thread builds, then composite via the same compositeLayers() routine,
// just bound to a one-shot offscreen FBO instead of the back buffer.
//
// Returns RGBA8 pixels (top-down — already V-flipped from the GL readback).
// Empty vector on failure or when the renderer is non-GPU.
std::vector<uint8_t> Engine::renderUnifiedToPixels() {
    if (!document_ || !gl_) return {};
    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia) return {};

    int w = viewportWidth_, h = viewportHeight_;

    // 1. Bind WebGL canvas FBO before firing rAF + scene graph render.
    webgl::WebGL2RenderingContext* activeWebGL = nullptr;
    if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
    if (activeWebGL) activeWebGL->bindCanvasFBO();

    // Scaled clock + pause gate, same as the frame loop: capturing a paused
    // app must not run its rAF game logic — the composite below re-samples
    // the last-drawn textures, so the capture shows the frozen frame.
    if (!timePaused_) timers_->fireAnimationFrames(engineNowMs_);
    jsRuntime_->executePendingJobs();

    if (activeWebGL) activeWebGL->unbindCanvasFBO();

    // 2. Materialize HtmlNodes + render scene graphs (windowed path runs both
    //    on the main thread before signaling raster).
#if BRO_WITH_3D
    for (auto& sg : sceneGraphs_) {
        if (sg.graph) sg.graph->materializeHtmlNodes(skia);
    }
    for (auto& sg : sceneGraphs_) {
        if (sg.graph) sg.graph->render();
    }
#endif  // BRO_WITH_3D

    // 3. Rasterize canvas scenes into their per-canvas FBOs; prune detached.
    for (auto& cs : canvasScenes_) {
        cs->setViewportScroll(scrollY_);
        cs->rasterize(gl_.get());
    }
    canvasScenes_.erase(
        std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
            [](auto& cs) { return cs->isDetached(); }),
        canvasScenes_.end());

    // 4. Tick + lay out system panels for this frame (raster thread does
    //    this inside buildSystemPanelLayers, but layoutSystemPanels reads
    //    isDirty so it's safe to call ahead).
    if (isSystemVisible()) tickSystemPanels(virtualTime_);

    // 5. Record commands on this (main) thread, then replay against the live
    //    Skia renderer — same record/replay split the windowed raster thread
    //    uses, just both halves on one thread. App layer surfaces are
    //    content-sized; compositeLayers places them at (0, insetTop).
    std::vector<UILayer> appLayers, systemLayers;
    render::CommandBuffer appCmds, sysCmds;
    int insetTop = contentTop();
    int cw = std::max(1, w - contentRight());
    int ch = std::max(1, h - insetTop - contentBottom());
    // Snapshot the Selection geometry before recording — recordAppLayers draws
    // the highlight from the snapshot (the windowed loop does this right
    // before signaling raster; without it headless GPU captures never showed
    // the document selection at all).
    updateSelectionSnapshot();
    skia->beginFrame(w, h);
    recordAppLayers(appCmds, w, h,
                    insetTop, contentRight(), contentBottom(), scrollY_);
    recordSystemPanelLayers(sysCmds, w, h);
    recordIframeLayers();
    replayAppLayers(skia, appCmds,
                    screenshotHtmlPool_, screenshotHtmlPoolW_, screenshotHtmlPoolH_,
                    cw, ch, appLayers);
    replaySystemPanelLayers(skia, sysCmds,
                            screenshotSystemPool_, screenshotSystemPoolW_,
                            screenshotSystemPoolH_,
                            w, h, systemLayers);
    replayIframeLayers(skia);
    skia->endFrame();

    // 6. Compositing FBO target.
    GLuint compositeFBO = 0, compositeTex = 0;
    glGenFramebuffers(1, &compositeFBO);
    compositeTex = gl_->createTexture2D(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glBindFramebuffer(GL_FRAMEBUFFER, compositeFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, compositeTex, 0);

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // 7. App layers → system layers (matches windowed pipeline).
    //    App layers place at (0, insetTop) with content dims.
    compositeLayers(appLayers, compositeFBO, insetTop, cw, ch);
    compositeLayers(systemLayers, compositeFBO);

    // 8. Readback.
    std::vector<uint8_t> pixels(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &compositeFBO);
    gl_->deleteTexture(compositeTex);

    if (activeWebGL) activeWebGL->restoreState();

    // 9. Flip vertically (OpenGL reads bottom-to-top, PNG/consumers expect top-down).
    int rowBytes = w * 4;
    std::vector<uint8_t> row(rowBytes);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* top = pixels.data() + y * rowBytes;
        uint8_t* bot = pixels.data() + (h - 1 - y) * rowBytes;
        memcpy(row.data(), top, rowBytes);
        memcpy(top, bot, rowBytes);
        memcpy(bot, row.data(), rowBytes);
    }

    return pixels;
}

bool Engine::screenshot(const std::string& path) {
    if (!document_) return false;

    // GPU path: unified pipeline (same layer list + compositeLayers as windowed).
    if (gl_ && dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        auto pixels = renderUnifiedToPixels();
        if (pixels.empty()) return false;
        int w = viewportWidth_, h = viewportHeight_;
        return broimage::encode_png_file(path, pixels.data(), w, h, 4);
    }

    // CPU path: fire rAF (with WebGL FBO bound), then draw directly to the
    // raster Skia surface and save via broimage.
    {
        webgl::WebGL2RenderingContext* activeWebGL = nullptr;
        if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
        if (activeWebGL) activeWebGL->bindCanvasFBO();
        if (!timePaused_) timers_->fireAnimationFrames(engineNowMs_);
        jsRuntime_->executePendingJobs();
        if (activeWebGL) activeWebGL->unbindCanvasFBO();
    }

    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    // The app document (and everything anchored to it) draws in content
    // space, same as the GPU layer path; the single window-space translation
    // by the engine-reserved inset happens here, around the whole block.
    renderer_->save();
    renderer_->translate(0.0f, static_cast<float>(contentTop()));

    // Composite canvas scenes (behind HTML) — Skia surface blit.
    // getScreenRect() is content space (layout − document scroll).
    for (auto& cs : canvasScenes_) {
        float cx, cy, cw, ch;
        cs->getScreenRect(cx, cy, cw, ch);
        if (cs->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> img = cs->surface()->makeImageSnapshot();
                if (img) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(img, cx, cy, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    // Render HTML/CSS overlay on top (content space: root offset is just the
    // document scroll, so lastDrawPos_ / layer coordinates match every mode).
    drawTraversal_->draw(document_->documentElement(),
                         0, -scrollY_,
                         contentWidth(), contentHeight(), /*viewportTop=*/0);

    // Selection highlight on top of HTML text.
    updateSelectionSnapshot();
    drawSelectionHighlight(renderer_.get(), -scrollY_);

    renderer_->restore();

    // System panels on top of the app content
    if (isSystemVisible()) {
        tickSystemPanels(virtualTime_);
        layoutSystemPanels(*textMetrics_);
        drawSystemPanels(renderer_.get(), *drawTraversal_);
    }

    renderer_->endFrame();

    return renderer_->saveScreenshot(path);
}

std::vector<uint8_t> Engine::capturePixels() {
    if (!document_) return {};

    // GPU path: unified pipeline (same layer list + compositeLayers as windowed).
    if (gl_ && dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        return renderUnifiedToPixels();
    }

    // CPU path: rAF + WebGL bind/unbind, then render directly to the Skia surface.
    {
        webgl::WebGL2RenderingContext* activeWebGL = nullptr;
        if (!webglEntries_.empty()) activeWebGL = webglEntries_[0].context.get();
        if (activeWebGL) activeWebGL->bindCanvasFBO();
        if (!timePaused_) timers_->fireAnimationFrames(engineNowMs_);
        jsRuntime_->executePendingJobs();
        if (activeWebGL) activeWebGL->unbindCanvasFBO();
    }

    renderer_->beginFrame(viewportWidth_, viewportHeight_);
    renderer_->clear({0, 0, 0, 255});

    // Content-space block, translated once by the inset — see screenshot().
    renderer_->save();
    renderer_->translate(0.0f, static_cast<float>(contentTop()));

    // Composite canvas scenes — Skia surface blit (content space)
    for (auto& cs : canvasScenes_) {
        float cx, cy, cw, ch;
        cs->getScreenRect(cx, cy, cw, ch);
        if (cs->surface()) {
            auto* appCanvas = renderer_->getCanvas();
            if (appCanvas) {
                sk_sp<SkImage> img = cs->surface()->makeImageSnapshot();
                if (img) {
                    SkPaint paint;
                    paint.setBlendMode(SkBlendMode::kSrcOver);
                    appCanvas->drawImage(img, cx, cy, SkSamplingOptions(), &paint);
                }
            }
        }
    }

    drawTraversal_->draw(document_->documentElement(),
                         0, -scrollY_,
                         contentWidth(), contentHeight(), /*viewportTop=*/0);

    // App-context overlays anchor in content space (lastDrawPos_).
    overlayMgr_.drawIfContext(OverlayContext::App, renderer_.get());

    renderer_->restore();

    if (isSystemVisible()) {
        tickSystemPanels(virtualTime_);
        layoutSystemPanels(*textMetrics_);
        drawSystemPanels(renderer_.get(), *drawTraversal_);
    }

    renderer_->endFrame();
    return renderer_->capturePixels();
}

bool Engine::screenshot(const std::string& path, int cx, int cy, int cw, int ch) {
    auto pixels = capturePixels();
    if (pixels.empty()) return false;

    int fw = viewportWidth_, fh = viewportHeight_;

    // Clamp crop rect to viewport bounds
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > fw) cw = fw - cx;
    if (cy + ch > fh) ch = fh - cy;
    if (cw <= 0 || ch <= 0) return false;

    // Extract cropped region
    std::vector<uint8_t> cropped(cw * ch * 4);
    for (int y = 0; y < ch; ++y) {
        const uint8_t* src = pixels.data() + ((cy + y) * fw + cx) * 4;
        uint8_t* dst = cropped.data() + y * cw * 4;
        memcpy(dst, src, cw * 4);
    }

    return broimage::encode_png_file(path, cropped.data(), cw, ch, 4);
}

dom::Element* Engine::querySelector(const std::string& selector) const {
    if (!document_) return nullptr;

    // Handle #id shorthand
    if (!selector.empty() && selector[0] == '#') {
        return document_->getElementById(selector.substr(1));
    }

    return document_->querySelector(selector);
}

// overlayQuerySelector() and overlayPanelNames() are in system_panels.cpp.

void Engine::dispatchClickOn(dom::Element* target) {
    if (!target || !jsRuntime_) return;
    if (document_) document_->setActiveElement(target);
    dom::MouseEvent event("click");
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

#if BRO_WITH_3D
scene::CullStats Engine::sceneCullStats() const {
    scene::CullStats sum;
    for (const auto& sg : sceneGraphs_) {
        if (!sg.graph) continue;
        const scene::CullStats& s = sg.graph->cullStats();
        sum.meshDrawn        += s.meshDrawn;
        sum.meshCulled       += s.meshCulled;
        sum.instancedDrawn   += s.instancedDrawn;
        sum.instancedCulled  += s.instancedCulled;
        sum.splatDrawn       += s.splatDrawn;
        sum.splatCulled      += s.splatCulled;
        sum.particlesDrawn   += s.particlesDrawn;
        sum.particlesCulled  += s.particlesCulled;
        sum.billboardsDrawn  += s.billboardsDrawn;
        sum.billboardsCulled += s.billboardsCulled;
        sum.shadowDrawn         += s.shadowDrawn;
        sum.shadowCulled        += s.shadowCulled;
        sum.shadowTilesTotal    += s.shadowTilesTotal;
        sum.shadowTilesRendered += s.shadowTilesRendered;
        sum.shadowTilesCached   += s.shadowTilesCached;
    }
    return sum;
}
#endif

} // namespace bro::engine
