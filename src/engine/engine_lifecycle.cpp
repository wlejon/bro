#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/matchmedia_bindings.h"
#include "js/audio_bindings.h"
#include "js/audio_scene_sync.h"
#include "js/storage_bindings.h"
#include "js/webgl2_bindings.h"
#include "js/scene_bindings.h"
#include "js/worker.h"
#include "js/async_job.h"
#if BRO_WITH_PHYSICS
#include "js/physics_bindings.h"
#endif
#include "js/server_bindings.h"
#include "js/net_bindings.h"
#include "js/steam_bindings.h"
#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/terrain_bindings.h"
#include "js/tile_bindings.h"
#include "js/custom_elements.h"
#include "js/wake_bindings.h"
#include "js/gesture_bindings.h"
#include "js/kws_bindings.h"
#if BRO_WITH_SOUNDML
#include "js/listen_host.h"  // fat header (pulls brosoundml/brotensor)
#endif
#include "js/sense_bindings.h"
#include "js/mic_bindings.h"
#include "js/stt_bindings.h"
#include "js/lm_bindings.h"
#include "js/tts_bindings.h"
#include "js/diar_bindings.h"
#include "js/rave_bindings.h"
#include "layout/box.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "render/skia_backend.h"
#include "render/recording_renderer.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "audio_inference/audio_inference.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#include "steam/steam_service.h"
#include "webgl/webgl2_context.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "render/renderer.h"
#include "render/gl_context.h"
#include "layout/draw_traversal.h"
#if BRO_WITH_3D
#include "engine/gizmo.h"
#endif
#include <broaudio/engine.h>
#if BRO_WITH_TENSOR
#include <brotensor/runtime.h>
#endif
#include "util/interrupt.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <algorithm>
#include <cmath>

namespace bro::engine {
void Engine::stopBackgroundServices() {
#if BRO_WITH_NET
    netService_.reset();
#endif
    steamService_.reset();
}

void Engine::shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    // Teardown is beginning: flip the process-wide interrupt flag (without the
    // repeated-Ctrl+C hard-exit escalation) so every in-flight model op aborts
    // at its next cooperative-cancel poll. The joins below would otherwise
    // block on long synchronous native inference calls — the JS interrupt
    // can't break a native call — leaving the app unresponsive, and a user
    // force-close then kills threads mid-CUDA-dispatch (see util/interrupt.h:
    // that has bugchecked the machine via nvlddmkm). Windowed close and Ctrl+C
    // already set the flag before we get here; this covers headless
    // script-end and error-path teardown.
    util::beginShutdown();

    // Cancel + join any in-flight async inference jobs (bro.lm/stt/tts) and
    // free their callbacks on this (the owning) thread. Must precede the
    // runtime teardown, and must happen through here rather than ~AsyncJob:
    // that dtor joins the worker WITHOUT cancelling it first, so an in-flight
    // generate would block teardown until the model finished on its own.
    if (jsRuntime_) js::shutdownAsyncJobs(jsRuntime_->getContext());

#if BRO_WITH_PHYSICS
    if (physicsWorld_) physicsWorld_->shutdown();
#endif

    if (layoutPipeline_) layoutPipeline_->postShutdown();
    if (layoutThread_.joinable()) layoutThread_.join();

    if (framePresenter_) framePresenter_->postShutdown();
    if (rasterThread_.joinable()) rasterThread_.join();

    if (rasterGLContext_) {
        SDL_GL_DestroyContext(rasterGLContext_);
        rasterGLContext_ = nullptr;
    }

    // Release every threaded scene's GPU resources on the shared worker (where
    // they live), then stop the worker, before GL context cleanup.
    if (canvasRasterThread_ && canvasRasterThread_->started()) {
        for (auto& cs : canvasScenes_) {
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        }
        for (auto& cs : canvasScenesDetached_) {
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        }
        canvasRasterThread_->stop();
    }
    // NB: the scenes themselves are deliberately NOT destroyed here — only
    // their GPU resources are released. ~Engine() must first sever the
    // Element->CanvasScene back-links (the Elements are still alive), or
    // ~Element's onBackingElementDestroyed hook fires against a freed scene.

    // Release SDL gamepad handles (no-op for virtual pads / none connected).
    closeAllGamepads();

    // No-op if run() never added it (headless/server never reach that code).
    removeModalEventWatch();
}

Engine::~Engine() {
    // Quiesce the worker threads and GPU contexts. run() already called this on
    // the windowed path; it is a no-op then. Headless and Server early-return
    // out of run() before its shutdown, so for them this IS the shutdown.
    shutdown();

    // Join brotensor's CPU worker threads now, deterministically, while the
    // rest of the process is still in a normal running state. Left to its
    // own Meyers-singleton destructor, this would only happen during the
    // process's static-destruction phase — by which point RtlExitUserProcess
    // has already suspended every other thread. A worker suspended mid-op
    // while holding some global lock (e.g. the Debug CRT's iterator-checking
    // mutex, taken by any std::vector destructor — including unrelated
    // thread_local scratch buffers like brogameagent's NavGrid::findPath)
    // can then deadlock the main thread's own exit-time TLS destructors
    // waiting on that same lock forever, which Windows eventually resolves
    // by force-killing the process. Reproduced via cdb: select ai-arena's
    // ExIt Net agent (its first MCTS search is what spins up the pool),
    // click Reset once, let the process exit normally.
#if BRO_WITH_TENSOR
    brotensor::shutdown();
#endif

    // Release screenshot pool surfaces while the main GL context is still
    // current. Safe to skip if the pool is empty (windowed mode never uses it).
    if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        for (auto& ps : screenshotHtmlPool_) skia->destroyGPUSurface(ps);
        screenshotHtmlPool_.clear();
        for (auto& ps : screenshotSystemPool_) skia->destroyGPUSurface(ps);
        screenshotSystemPool_.clear();

        // Iframe sub-doc surfaces, for the HEADLESS half of the story: there is
        // no raster thread, so screenshot() replays the sub-docs with THIS (the
        // main) renderer — the surfaces belong to this context and must be
        // released here, before destroyAllIframes() below drops the IframeDocs.
        // Windowed builds them on the raster thread's context instead, and that
        // thread already emptied them on its way out, so this is a no-op there.
        for (auto& d : iframeDocs_) {
            if (!d) continue;
            skia->destroyGPUSurface(d->surface);
            d->surfW = d->surfH = 0;
            d->fboTexture = 0;
        }
        drainIframeSurfaceFrees(skia);
    }

    // Release menu handler JS references before the runtime tears down.
    menuBar_.releaseHandlers();

    // Clear terrain managers before destroying scene graphs — their destructors
    // call SceneGraph::destroyNode(), which crashes if the graph is already gone.
#if BRO_WITH_3D
    if (jsRuntime_) {
        js::TerrainBindings::cleanup(jsRuntime_->getContext());
        js::TileBindings::cleanup(jsRuntime_->getContext());
    }

    // Destroy scene graphs before canvas scenes (they hold canvas pointers)
    sceneGraphs_.clear();
#endif

    // shutdown() already released the threaded scenes' GPU resources on the
    // canvas worker and stopped it; the scenes themselves are still alive.
    //
    // Sever every Element->CanvasScene back-pointer before destroying the
    // scenes. The Document (and its Elements) outlives this point — it is reset
    // far below — so a surviving back-pointer means ~Element invokes its
    // on-destroy hook (CanvasScene::onBackingElementDestroyed) against freed
    // memory.
    //
    // This walks the DOCUMENT, not the scene list, because the two sides are
    // not reliably 1:1: an Element can still name a scene that the scene no
    // longer names back (churn through create/remove reuses Element addresses,
    // and a scene whose Element was finalized nulls its own userdata). Severing
    // from the scene side alone left exactly those Elements dangling, and
    // tests/canvas/test_canvas_detach_churn.js faulted here roughly half the
    // time. Elements queued for deferred free are still live memory that
    // ~Document will destroy, so forEachLiveElement covers them too.
    if (document_) {
        document_->forEachLiveElement(
            [](dom::Element* el) { el->setCanvasScene(nullptr); });
    }
    canvasScenesDetached_.clear();
    canvasScenes_.clear();
    canvasSceneRegistry_.clear();
    canvasRasterThread_.reset();

    // WebGL contexts (unique_ptr destruction handles cleanup)
    webglEntries_.clear();
    destroyAllIframes();
    destroySystemPanels();

    // 0. Clear ElementRefAdapter cache (holds raw pointers to elements)
    layout::ElementRefAdapter::clearCache();

    // 1. Clear timers (they hold JS callbacks)
    if (timers_ && jsRuntime_) {
        timers_->clearAll(jsRuntime_->getContext());
    }

    // 2. Clear JS bindings
    if (jsRuntime_) {
        JSContext* ctx = jsRuntime_->getContext();
        js::setElementFinalizerShutdown(true);
        // Wake/mic cleanup must run before audioEngine_.reset() — it removes
        // the mic taps that feed the audio-inference rings on the audio thread,
        // and unregisters the wake task. Leaving a tap attached lets the audio
        // thread keep writing rings, and leaving the inference worker running
        // lets it keep launching CUDA work into a brotensor context that's being
        // torn down by static destructors — which has produced kernel-level
        // driver faults on exit.
#if BRO_WITH_SOUNDML
        js::cleanupWakeBindings(ctx);
        js::cleanupKwsBindings(ctx);
        js::cleanupSenseBindings(ctx);
        js::cleanupGestureBindings(ctx);
        // Both listen-host members are detached now (their cleanups above ran
        // Set*(nullptr), which tears down the shared tap + task on the last
        // detach); this just drops the host's subsystem pointers.
        js::shutdownListenHost();
#endif
        js::cleanupMicBindings(ctx);
        // Join the audio-inference worker now: the tap is detached (no more ring
        // writes) and the wake task unregistered, so the worker drains its final
        // command, destroys the model (its CUDA frees run on the worker thread),
        // and exits before audio + brotensor teardown below.
        if (audioInference_) audioInference_->shutdown();
#if BRO_WITH_SOUNDML
        js::cleanupSttBindings(ctx);
        js::cleanupTtsBindings(ctx);
        js::cleanupDiarBindings(ctx);
        js::cleanupRaveBindings(ctx);
#endif
#if BRO_WITH_LM
        js::cleanupLmBindings(ctx);
#endif
        js::cleanupWorkerBindings(ctx);
        js::ServerBindings::cleanup(ctx);
        js::NetBindings::cleanup(ctx);
        // Frees the thread-local Steam state's JS refs (event handlers,
        // pending promises) before the runtime goes, and destroys its
        // service subscriber — same ordering performAppReload uses.
        js::SteamBindings::cleanup(ctx);

        // Now — and not before — the net/Steam service threads can go. Their
        // bindings above hold raw service pointers and call back into them
        // (NetBindings::cleanup -> service->destroySubscriber()), so stopping
        // the services any earlier is a use-after-free. It faults or hangs on
        // a freed condvar depending on timing, which is exactly what headless
        // did for months: it called stopBackgroundServices() before ~Engine(),
        // and the _exit() below hid the wreckage.
        stopBackgroundServices();
#if BRO_WITH_PHYSICS
        js::PhysicsBindings::cleanup(ctx);
#endif
#if BRO_WITH_3D
        js::TerrainBindings::cleanup(ctx);
        js::TileBindings::cleanup(ctx);
        js::MeshBindings::cleanup(ctx);
        js::RiggingBindings::cleanup(ctx);
#endif
        js::AIBindings::cleanup(ctx);  // no-op stub when GAMEAI off
#if BRO_WITH_3D
        js::SceneBindings::cleanup(ctx);
#endif
        js::AudioBindings::cleanup(ctx);
        js::StorageBindings::cleanup(ctx);
        if (gl_) {
            js::WebGL2Bindings::cleanup(ctx);
        }
        js::cleanupCustomElements(ctx);

        // Free cached compiled function (holds a GC-tracked JSValue).
        if (!JS_IsUndefined(observerCheckFn_)) {
            JS_FreeValue(ctx, observerCheckFn_);
            observerCheckFn_ = JS_UNDEFINED;
        }

        // Clean up global properties (prevents leaked references).
        // Delete document and elem_map first, in that order — the map holds JS
        // refs to elements whose finalizers call freeNode(); deleting the map
        // first can free elements still referenced by the document tree,
        // causing use-after-free when the document wrapper is subsequently
        // collected.
        JSValue global = JS_GetGlobalObject(ctx);
        JSAtom a1 = JS_NewAtom(ctx, "document");
        JSAtom a2 = JS_NewAtom(ctx, "__bro_elem_map");
        JSAtom a3 = JS_NewAtom(ctx, "console");
        JS_DeleteProperty(ctx, global, a1, 0);
        JS_DeleteProperty(ctx, global, a2, 0);
        JS_DeleteProperty(ctx, global, a3, 0);
        JS_FreeAtom(ctx, a1);
        JS_FreeAtom(ctx, a2);
        JS_FreeAtom(ctx, a3);

        // Delete every remaining own property on globalThis. App code pins
        // game state on `window.foo = ...` (e.g. stompworld's window.__SW),
        // and those refs transitively keep C++-bound wrappers (BestCrop pools,
        // etc.) alive past JS_FreeRuntime's leak check. Clearing the globals
        // lets the cycle GC sweep it all before we tear the runtime down.
        // Skip standard JS built-ins (Object/Array/Function/etc. — anything
        // owned by the engine itself) by only touching enumerable strings.
        {
            JSPropertyEnum* props = nullptr;
            uint32_t len = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &len, global,
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < len; ++i) {
                    JS_DeleteProperty(ctx, global, props[i].atom, 0);
                    JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, global);
        js::DomBindings::cleanup(ctx);
        jsRuntime_->executePendingJobs();
        JS_RunGC(jsRuntime_->getRuntime());
        // Second GC pass — typed arrays and closures may form reference
        // chains that need multiple collections to fully release.
        jsRuntime_->executePendingJobs();
        JS_RunGC(jsRuntime_->getRuntime());
    }

    // 3. GL cleanup (windowed only)
    {
        auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
        for (int i = 0; i < 2; ++i) {
            if (skia) {
                for (auto& ps : htmlSurfacePool_[i]) skia->destroyGPUSurface(ps);
                for (auto& ps : systemSurfacePool_[i]) skia->destroyGPUSurface(ps);
            }
            htmlSurfacePool_[i].clear();
            systemSurfacePool_[i].clear();
        }
    }
    if (uiQuadVBO_) { glDeleteBuffers(1, &uiQuadVBO_); uiQuadVBO_ = 0; }
    if (uiQuadVAO_) { glDeleteVertexArrays(1, &uiQuadVAO_); uiQuadVAO_ = 0; }

    // 4. Release layout resources before document
    drawTraversal_.reset();

    // Clean up per-runtime DomBindings state before the runtime is freed.
    if (jsRuntime_) {
        js::DomBindings::cleanupRuntime(jsRuntime_->getRuntime());
    }
    // Release JS function references stored in the gizmo callbacks before
    // tearing down the runtime — gizmo_ is a member that outlives this
    // destructor body in default member-destruction order, so its dtor
    // would otherwise see a dead JSContext (and the runtime would abort
    // with a leak-detected assertion before reaching that point).
#if BRO_WITH_3D
    if (gizmo_) gizmo_->clearCallbacks();
    // Tear down scene graphs before the JS runtime — AgentBinding owns
    // JsThinkHook which holds JS_DupValue'd refs to world/agent JS objects.
    // If the runtime dies first, those JS_FreeValue calls run on a dead
    // context, refs never release, and JS_FreeRuntime asserts on leaks.
    sceneGraphs_.clear();
#endif
    // Destroy JS runtime BEFORE document — JS_FreeRuntime() runs GC finalizers
    // that dereference Element pointers, so elements must still be alive.
    // Audio engine must also outlive JS runtime because VoiceAllocator/MidiInput
    // destructors reference it (removeVoice, close).
    jsRuntime_.reset();
#if BRO_WITH_PHYSICS
    physicsWorld_.reset();
#endif
    // Worker already joined above (during binding cleanup); this just frees it.
    audioInference_.reset();
    // Drop the scene-emitter registry's engine pointer before the audio
    // engine dies (the registry itself holds only weak tokens + ids).
    js::shutdownAudioSceneSync();
    audioEngine_.reset();
    document_.reset();
    timers_.reset();
    renderer_.reset();
}

void Engine::handleResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    uiDirty_ = true;
    hasRenderedOnce_ = false;
    // The app document draws in content space (origin 0,0; the compositor
    // applies the engine inset when placing the layers), so viewportTop is 0.
    drawTraversal_->setViewport(contentWidth(), contentHeight(), 0);
    // WebGL canvases resize based on element layout, not viewport — handled per-frame
    {
        resizeSystemPanels(w, h);
    }
    int cw = contentWidth();
    int ch = contentHeight();
    if (document_) {
        // Re-evaluate @media blocks against the new viewport before restyling.
        document_->setMediaViewport(static_cast<float>(cw), static_cast<float>(ch));
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_.get());
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(cw), static_cast<float>(ch), *textMetrics_);
        if (document_->documentElement()) {
            auto& box = document_->documentElement()->layoutBox();
            documentHeight_ = box.marginBox().height;
        }
        // Clamp scroll after resize
        float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(ch));
        scrollY_ = std::clamp(scrollY_, 0.0f, maxScroll);
    }

    // Update JS globals and dispatch resize event to window listeners
    if (jsRuntime_) {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);

        // Update innerWidth / innerHeight (excludes engine-reserved insets so
        // apps see a web-like viewport that matches their layout area).
        JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, cw));
        JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, ch));
        // Refresh devicePixelRatio too — handleDisplayScaleChanged routes
        // through here so a display-scale change reaches apps as the same
        // resize event browsers fire (apps re-read window.devicePixelRatio
        // in their resize handler).
        JS_SetPropertyStr(ctx, global, "devicePixelRatio",
                          JS_NewFloat64(ctx, displayScale_));

        // Auto-size every <canvas> that didn't declare a fixed buffer size.
        // Authors opt out by setting the width/height HTML attributes; those
        // canvases keep whatever bitmap size they declared. We update the
        // CanvasScene directly (not via the JS width/height setters) so the
        // HTML attribute stays absent on auto-sized canvases — that way the
        // "no attribute => auto-size" signal remains stable across resizes.
        if (document_) {
            for (auto* el : document_->querySelectorAll("canvas")) {
                if (!el) continue;
                if (el->hasAttribute("width") || el->hasAttribute("height"))
                    continue;
                auto* cs = static_cast<bro::canvas::CanvasScene*>(el->canvasScene());
                if (!cs) continue;
                cs->setIntrinsicWidth(cw);
                cs->setIntrinsicHeight(ch);
                cs->reset();
            }
        }

        // Dispatch resize event to window listeners
        JSValue dispatch = JS_GetPropertyStr(ctx, global, "__bro_dispatch_window_event");
        if (JS_IsFunction(ctx, dispatch)) {
            JSValue evtType = JS_NewString(ctx, "resize");
            JSValue evt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, evt, "type", JS_NewString(ctx, "resize"));
            JS_SetPropertyStr(ctx, evt, "target", JS_DupValue(ctx, global));
            JSValue dArgs[2] = { evtType, evt };
            JSValue ret = JS_Call(ctx, dispatch, global, 2, dArgs);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, evtType);
            JS_FreeValue(ctx, evt);
        }
        JS_FreeValue(ctx, dispatch);

        JS_FreeValue(ctx, global);
        jsRuntime_->executePendingJobs();
    }

    // The app document was restyled synchronously above, so matchMedia change
    // events can fire now (same task as the resize, like browsers). Realms
    // whose media-triggered restyle hasn't landed yet (iframes / system panels
    // on a scheme flip) defer to the frame/flush drain via the per-realm gate.
    deliverMediaQueryChangesAllRealms();
}

void Engine::handleDisplayScaleChanged() {
    // Windowed only: headless pins devicePixelRatio to 1.0 so test output
    // never depends on the desktop the suite runs on.
    if (displayMode_ != DisplayMode::Windowed || !window_) return;
    float scale = window_->getDisplayScale();
    if (std::fabs(scale - displayScale_) < 1e-3f) return;
    displayScale_ = scale;

    // Refresh devicePixelRatio in the secondary realms directly — iframes and
    // system panels share the app's window, so they see the same scale. The
    // app realm is refreshed by handleResize below (which also dispatches the
    // window resize event apps re-read the ratio from, as browsers do).
    // htmlayout's media evaluator has no resolution/min-resolution feature,
    // so there are no MediaQueryLists to re-evaluate for a scale change.
    auto refreshRealm = [&](JSContext* ctx) {
        if (!ctx) return;
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "devicePixelRatio",
                          JS_NewFloat64(ctx, displayScale_));
        JS_FreeValue(ctx, global);
    };
    for (auto& doc : iframeDocs_) {
        if (doc) refreshRealm(doc->jsCtx);
    }
    for (auto& doc : systemDocs_) refreshRealm(doc.jsCtx);

    handleResize(viewportWidth_, viewportHeight_);
}

std::string Engine::effectiveColorScheme() const {
    if (settings_) {
        const std::string& pref = settings_->appearance().colorScheme;
        if (pref == "light" || pref == "dark") return pref;
    }
    // "system": follow the OS theme. UNKNOWN (no video driver, or a platform
    // without a theme concept) counts as light — the web default.
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? "dark" : "light";
}

void Engine::applyColorScheme() {
    const std::string scheme = effectiveColorScheme();
    if (document_) document_->setMediaColorScheme(scheme);
    for (auto& doc : iframeDocs_) {
        if (doc && doc->document) doc->document->setMediaColorScheme(scheme);
    }
    for (auto& doc : systemDocs_) {
        if (doc.document) doc.document->setMediaColorScheme(scheme);
    }
}

void Engine::deliverMediaQueryChangesAllRealms() {
    if (jsRuntime_) js::deliverMediaQueryChanges(jsRuntime_->getContext());
    for (auto& d : iframeDocs_) {
        if (d && d->jsCtx) js::deliverMediaQueryChanges(d->jsCtx);
    }
    for (auto& doc : systemDocs_) {
        if (doc.jsCtx) js::deliverMediaQueryChanges(doc.jsCtx);
    }
}

} // namespace bro::engine
