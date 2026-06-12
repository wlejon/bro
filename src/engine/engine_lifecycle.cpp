#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "js/webgl2_bindings.h"
#include "js/scene_bindings.h"
#include "js/worker.h"
#include "js/physics_bindings.h"
#include "js/server_bindings.h"
#include "js/net_bindings.h"
#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/terrain_bindings.h"
#include "js/custom_elements.h"
#include "js/wake_bindings.h"
#include "js/kws_bindings.h"
#include "js/listen_host.h"
#include "js/sense_bindings.h"
#include "js/mic_bindings.h"
#include "js/stt_bindings.h"
#include "js/lm_bindings.h"
#include "js/tts_bindings.h"
#include "js/rave_bindings.h"
#include "layout/box.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "render/skia_backend.h"
#include "render/recording_renderer.h"
#include "scene/scene_graph.h"
#include "physics/physics_world.h"
#include "audio_inference/audio_inference.h"
#include "net/net_service.h"
#include "webgl/webgl2_context.h"
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "render/renderer.h"
#include "render/gl_context.h"
#include "layout/draw_traversal.h"
#include "engine/gizmo.h"
#include <broaudio/engine.h>
#include "util/interrupt.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <algorithm>

namespace bro::engine {
Engine::~Engine() {
    // Teardown is beginning: flip the process-wide interrupt flag (without the
    // repeated-Ctrl+C hard-exit escalation) so every in-flight model op aborts
    // at its next cooperative-cancel poll. The worker joins below would
    // otherwise block on long synchronous native inference calls — the JS
    // interrupt can't break a native call — leaving the app unresponsive,
    // and a user force-close then kills threads mid-CUDA-dispatch (see
    // util/interrupt.h: that has bugchecked the machine via nvlddmkm).
    // Windowed close and Ctrl+C already set the flag before we get here;
    // this covers headless script-end and error-path teardown.
    util::beginShutdown();

    // Release screenshot pool surfaces while the main GL context is still
    // current. Safe to skip if the pool is empty (windowed mode never uses it).
    if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        for (auto& ps : screenshotHtmlPool_) skia->destroyGPUSurface(ps);
        screenshotHtmlPool_.clear();
        for (auto& ps : screenshotSystemPool_) skia->destroyGPUSurface(ps);
        screenshotSystemPool_.clear();
    }

    // Ensure layout thread is stopped (safety — normally joined in run())
    if (layoutPipeline_) layoutPipeline_->postShutdown();
    if (layoutThread_.joinable()) layoutThread_.join();
    // Ensure raster thread is stopped (safety — normally joined in run())
    if (framePresenter_) framePresenter_->postShutdown();
    if (rasterThread_.joinable()) rasterThread_.join();
    if (rasterGLContext_) {
        SDL_GL_DestroyContext(rasterGLContext_);
        rasterGLContext_ = nullptr;
    }

    // Release menu handler JS references before the runtime tears down.
    menuBar_.releaseHandlers();

    // Clear terrain managers before destroying scene graphs — their destructors
    // call SceneGraph::destroyNode(), which crashes if the graph is already gone.
    if (jsRuntime_) {
        js::TerrainBindings::cleanup(jsRuntime_->getContext());
    }

    // Destroy scene graphs before canvas scenes (they hold canvas pointers)
    sceneGraphs_.clear();

    // Release threaded scenes' GPU resources on the shared worker and stop it
    // before destroying the scenes (the frame loop's shutdown normally did this
    // already; this is a no-op then, and a safety net for early-teardown paths).
    if (canvasRasterThread_ && canvasRasterThread_->started()) {
        for (auto& cs : canvasScenes_) {
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        }
        for (auto& cs : canvasScenesDetached_) {
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        }
        canvasRasterThread_->stop();
    }
    // Sever Element->scene back-links before destroying the scenes. The
    // Document (and its Elements) outlives this point — it is reset far below —
    // so its ~Element would otherwise invoke the on-destroy hook
    // (onBackingElementDestroyed) against a freed scene. The backing Elements
    // are still alive here, so this is safe to touch.
    for (auto& cs : canvasScenes_) {
        if (cs) if (auto* el = static_cast<dom::Element*>(cs->backingElement()))
            el->setCanvasScene(nullptr);
    }
    for (auto& cs : canvasScenesDetached_) {
        if (cs) if (auto* el = static_cast<dom::Element*>(cs->backingElement()))
            el->setCanvasScene(nullptr);
    }
    canvasScenesDetached_.clear();
    canvasScenes_.clear();
    canvasRasterThread_.reset();

    // WebGL contexts (unique_ptr destruction handles cleanup)
    webglEntries_.clear();
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
        js::cleanupWakeBindings(ctx);
        js::cleanupKwsBindings(ctx);
        js::cleanupSenseBindings(ctx);
        // Both listen-host members are detached now (their cleanups above ran
        // Set*(nullptr), which tears down the shared tap + task on the last
        // detach); this just drops the host's subsystem pointers.
        js::shutdownListenHost();
        js::cleanupMicBindings(ctx);
        // Join the audio-inference worker now: the tap is detached (no more ring
        // writes) and the wake task unregistered, so the worker drains its final
        // command, destroys the model (its CUDA frees run on the worker thread),
        // and exits before audio + brotensor teardown below.
        if (audioInference_) audioInference_->shutdown();
        js::cleanupSttBindings(ctx);
        js::cleanupLmBindings(ctx);
        js::cleanupTtsBindings(ctx);
        js::cleanupRaveBindings(ctx);
        js::cleanupWorkerBindings(ctx);
        js::ServerBindings::cleanup(ctx);
        js::NetBindings::cleanup(ctx);
        js::PhysicsBindings::cleanup(ctx);
        js::TerrainBindings::cleanup(ctx);
        js::MeshBindings::cleanup(ctx);
        js::RiggingBindings::cleanup(ctx);
        js::AIBindings::cleanup(ctx);
        js::SceneBindings::cleanup(ctx);
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
        if (skia) {
            for (auto& ps : htmlSurfacePool_) skia->destroyGPUSurface(ps);
            for (auto& ps : systemSurfacePool_) skia->destroyGPUSurface(ps);
        }
        htmlSurfacePool_.clear();
        systemSurfacePool_.clear();
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
    if (gizmo_) gizmo_->clearCallbacks();
    // Tear down scene graphs before the JS runtime — AgentBinding owns
    // JsThinkHook which holds JS_DupValue'd refs to world/agent JS objects.
    // If the runtime dies first, those JS_FreeValue calls run on a dead
    // context, refs never release, and JS_FreeRuntime asserts on leaks.
    sceneGraphs_.clear();
    // Destroy JS runtime BEFORE document — JS_FreeRuntime() runs GC finalizers
    // that dereference Element pointers, so elements must still be alive.
    // Audio engine must also outlive JS runtime because VoiceAllocator/MidiInput
    // destructors reference it (removeVoice, close).
    jsRuntime_.reset();
    physicsWorld_.reset();
    // Worker already joined above (during binding cleanup); this just frees it.
    audioInference_.reset();
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
    drawTraversal_->setViewport(contentWidth(), contentHeight(), contentTop());
    // WebGL canvases resize based on element layout, not viewport — handled per-frame
    {
        resizeSystemPanels(w, h);
    }
    int cw = contentWidth();
    int ch = contentHeight();
    if (document_) {
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_);
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
}
} // namespace bro::engine
