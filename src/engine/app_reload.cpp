// Top-level location.reload(): tear down the app document + its JS realm and
// rebuild both in the same Engine and window — the web's reload semantics.
//
// Shape of the problem: location.reload() is called FROM JS inside the very
// realm being destroyed, so requestAppReload() only queues. The actual work
// (performAppReload) runs at a point with no app-realm JS on the stack and,
// windowed, with the layout + raster workers idle — the frame loop's drain
// (engine_frame.cpp) or between the headless driver's evaluation units
// (headless/main.cpp). This mirrors exactly how iframe reloads defer to
// processPendingIframeReloads(); the top-level document just has far more
// engine state hanging off it.
//
// The teardown below is ~Engine()'s app-realm subset, in the same order and
// for the same reasons (see engine_lifecycle.cpp) — but it stops short of
// everything engine-level: worker threads keep running, GL contexts and
// surface pools stay, services (net/steam), the audio engine, the physics
// world, system panels, and the JS *runtime* all survive. Only the primary
// JSContext is swapped (js::Runtime::renewContext) and the app document
// rebuilt via initAppRealm() — the same function the constructor uses.

#include "engine/engine.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "js/window_host_bindings.h"
#include "js/window_bindings.h"
#include "js/settings_bindings.h"
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
#include "js/clipmap_bindings.h"
#include "js/terrain_bindings.h"
#include "js/tile_bindings.h"
#include "js/custom_elements.h"
#include "js/html_interfaces.h"
#include "js/image_bindings.h"
#include "js/wake_bindings.h"
#include "js/gesture_bindings.h"
#include "js/kws_bindings.h"
#include "js/sense_bindings.h"
#include "js/mic_bindings.h"
#include "js/stt_bindings.h"
#include "js/lm_bindings.h"
#include "js/tts_bindings.h"
#include "js/diar_bindings.h"
#include "js/rave_bindings.h"
#include "layout/element_ref_adapter.h"
#include "render/skia_backend.h"
#if BRO_WITH_3D
#include "engine/gizmo.h"
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/log.h"

#include <exception>

extern "C" {
#include "quickjs.h"
}

namespace bro::engine {

void Engine::requestAppReload() {
    // Server mode has no document/realm to reload; the polyfill hook is never
    // installed there, but keep the guard for direct callers.
    if (displayMode_ == DisplayMode::Server) return;
    // A bool, so a second location.reload() in the same frame coalesces.
    pendingAppReload_ = true;
    // Ensure a frame actually runs and reaches the drain point even if the
    // document is otherwise idle.
    uiDirty_ = true;
}

bool Engine::processPendingAppReload() {
    if (!pendingAppReload_) return false;
    pendingAppReload_ = false;
    performAppReload();
    return true;
}

void Engine::performAppReload() {
    if (!jsRuntime_) return;
    JSContext* oldCtx = jsRuntime_->getContext();
    LOG_INFO("location.reload(): reloading app '%s'", appDir_.c_str());

    // ── Steps that run INTO the old realm (it is still fully alive) ─────────

    // Pointer lock + any open overlay (dropdown / color picker) anchor into
    // the old document and may hold JS callbacks; release them while calling
    // back into old-realm JS is still legal.
    exitPointerLock();
    overlayMgr_.close();

    // Cancel + join in-flight async model jobs (bro.lm/stt/tts generate) and
    // free their old-realm callbacks. Same reason ~Engine does it first.
    js::shutdownAsyncJobs(oldCtx);

    // ── Engine chrome holding old-realm JS references ────────────────────────

    // App menu items' handlers die with the realm; drop back to the default
    // tree — the fresh realm re-adds its own items.
    menuBar_.releaseHandlers();
    resetMenuBarDefaults();
    onMenuChanged();

#if BRO_WITH_3D
    if (gizmo_) gizmo_->clearCallbacks();
#endif

    if (!JS_IsUndefined(observerCheckFn_)) {
        JS_FreeValue(oldCtx, observerCheckFn_);
        observerCheckFn_ = JS_UNDEFINED;
    }

    // Inspector selection + id map point into the old tree.
    inspector_.selected = nullptr;
    inspector_.pickerHover = nullptr;
    inspectorNodeMap_.clear();

    // ── 3D + terrain/tile state bound to the old realm ──────────────────────

#if BRO_WITH_3D
    // Terrain/tile managers before the graphs — their destructors call
    // SceneGraph::destroyNode() (same order as ~Engine).
    js::TerrainBindings::cleanup(oldCtx);
    js::ClipmapBindings::cleanup(oldCtx);
    js::TileBindings::cleanup(oldCtx);
    // Severs the Element->graph back-pointers on the way out; the old realm's
    // Elements outlive this point (the document is swapped further down).
    clearSceneGraphs();
#endif

    // ── Canvas scenes ────────────────────────────────────────────────────────
    // Release each threaded scene's GPU resources on the canvas worker that
    // owns them (sync RPC; the worker keeps running for the next realm), sever
    // the Element→scene back-links, then destroy. Mirrors shutdown()/~Engine.
    if (canvasRasterThread_ && canvasRasterThread_->started()) {
        for (auto& cs : canvasScenes_)
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        for (auto& cs : canvasScenesDetached_)
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
    }
    if (document_) {
        document_->forEachLiveElement(
            [](dom::Element* el) { el->setCanvasScene(nullptr); });
    }
    canvasScenesDetached_.clear();
    canvasScenes_.clear();
    canvasSceneRegistry_.clear();

    // WebGL contexts — GL objects on this thread's (main) context.
    webglEntries_.clear();

    // ── Iframe sub-documents ─────────────────────────────────────────────────
    // Their GPU surfaces belong to whichever context replayed them (raster
    // thread windowed, main renderer headless); route through the owner.
    for (auto& d : iframeDocs_)
        if (d) queueIframeSurfaceFree(std::move(d->surface));
    destroyAllIframes();
    pendingIframeReloads_.clear();
    iframeLoadFailed_.clear();
    iframeSyncNeeded_ = false;

    // ── Secondary window hosts ───────────────────────────────────────────────
    // The dying realm owns every handle; its windows go with it. No 'close'
    // events (the realm is being destroyed) — the handle refs release in the
    // binding cleanup below.
    destroyAllWindowHosts(/*notifyJs=*/false);
    if (displayMode_ == DisplayMode::Headless) {
        // No raster thread — the main renderer created the surfaces, free them
        // here. Windowed leaves the queue for the raster thread's next replay.
        if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get()))
            drainIframeSurfaceFrees(skia);
    }

    // ── Timers, caches, and per-document animation state ─────────────────────

    if (timers_) timers_->clearAll(oldCtx);
    layout::ElementRefAdapter::clearCache();
    transitionManager_.clearAll();
    animationManager_.clearAll();
    webAnimationManager_.clearAll();
    // The cached base command buffer + promoted sets reference old Elements.
    promotedElements_.clear();
    basePromotedSet_.clear();
    baseCommands_.clear();
    baseValid_ = false;
    appBaseDirty_ = true;
    lastLayoutContentW_ = lastLayoutContentH_ = -1;
    // Input state: handles self-heal, but stale pointer captures / touch
    // contacts would confuse the fresh realm's first interactions.
    pointerCaptures_.clear();
    touchContacts_.clear();

    // ── Per-context binding cleanup — same order as ~Engine ─────────────────

    // Element-wrapper finalizers may now run against Elements freed by the
    // document teardown below; the flag makes them skip the dereference
    // (identical to ~Engine's use).
    js::setElementFinalizerShutdown(true);

#if BRO_WITH_SOUNDML
    js::cleanupWakeBindings(oldCtx);
    js::cleanupKwsBindings(oldCtx);
    js::cleanupSenseBindings(oldCtx);
    js::cleanupGestureBindings(oldCtx);
    // NOT shutdownListenHost(): the shared listen host is engine-level and
    // survives realms (the cleanups above detached this realm's members).
#endif
    js::cleanupMicBindings(oldCtx);
    // NOT audioInference_->shutdown(): the worker is engine-level.
#if BRO_WITH_SOUNDML
    js::cleanupSttBindings(oldCtx);
    js::cleanupTtsBindings(oldCtx);
    js::cleanupDiarBindings(oldCtx);
    js::cleanupRaveBindings(oldCtx);
#endif
#if BRO_WITH_LM
    js::cleanupLmBindings(oldCtx);
#endif
    js::cleanupWorkerBindings(oldCtx);
    js::ServerBindings::cleanup(oldCtx);
    js::NetBindings::cleanup(oldCtx);
    // Steam state is a thread-local singleton (install refuses to run twice);
    // clear it so the fresh realm's install takes. ~Engine skips this only
    // because the whole thread is about to end there.
    js::SteamBindings::cleanup(oldCtx);
    // NOT stopBackgroundServices(): net/steam services survive; the fresh
    // realm's installs create new subscribers.
#if BRO_WITH_PHYSICS
    js::PhysicsBindings::cleanup(oldCtx);
#endif
#if BRO_WITH_3D
    js::MeshBindings::cleanup(oldCtx);
    js::RiggingBindings::cleanup(oldCtx);
#endif
    js::AIBindings::cleanup(oldCtx);
#if BRO_WITH_3D
    js::SceneBindings::cleanup(oldCtx);
#endif
    js::AudioBindings::cleanup(oldCtx);
    js::StorageBindings::cleanup(oldCtx);
    js::cleanupWindowHostBindings(oldCtx);
    js::cleanupWindowBindings(oldCtx);
    if (gl_) {
        js::WebGL2Bindings::cleanup(oldCtx);
    }
    js::cleanupCustomElements(oldCtx);
    js::cleanupHtmlInterfaces(oldCtx);
    js::ImageBindings::cleanup(oldCtx);

    // ── Release the old realm's globals, document, and context ──────────────
    // Same scrub ~Engine does: delete document + elem map first (order
    // matters — see engine_lifecycle.cpp), then every enumerable own property
    // so app state pinned on `window.foo = ...` releases its wrappers.
    {
        JSValue global = JS_GetGlobalObject(oldCtx);
        JSAtom a1 = JS_NewAtom(oldCtx, "document");
        JSAtom a2 = JS_NewAtom(oldCtx, "__bro_elem_map");
        JSAtom a3 = JS_NewAtom(oldCtx, "console");
        JS_DeleteProperty(oldCtx, global, a1, 0);
        JS_DeleteProperty(oldCtx, global, a2, 0);
        JS_DeleteProperty(oldCtx, global, a3, 0);
        JS_FreeAtom(oldCtx, a1);
        JS_FreeAtom(oldCtx, a2);
        JS_FreeAtom(oldCtx, a3);

        JSPropertyEnum* props = nullptr;
        uint32_t len = 0;
        if (JS_GetOwnPropertyNames(oldCtx, &props, &len, global,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < len; ++i) {
                JS_DeleteProperty(oldCtx, global, props[i].atom, 0);
                JS_FreeAtom(oldCtx, props[i].atom);
            }
            js_free(oldCtx, props);
        }
        JS_FreeValue(oldCtx, global);
    }
    js::DomBindings::cleanup(oldCtx);
    jsRuntime_->executePendingJobs();
    JS_RunGC(jsRuntime_->getRuntime());
    jsRuntime_->executePendingJobs();
    JS_RunGC(jsRuntime_->getRuntime());

    // Document before context (the iframe teardown order): ~Document fires
    // fireNodeDestroying for every element it still owns, nulling wrapper
    // opaques while the node storage is valid, so the context free below is
    // inert on element wrappers. (Note the ordering requirement: this must stay
    // ahead of JS_FreeContext, and the hook deliberately does not consult
    // s_doc_to_ctx, which DomBindings::cleanup above has already dropped.)
    document_.reset();
    documentHeight_ = 0.0f;
    scrollY_ = 0.0f;
    wheelResidualY_ = 0.0f;
    selectionDragging_ = false;
    selectionPastThreshold_ = false;

    // Swap the realm. renewContext frees oldCtx and creates the new primary.
    jsRuntime_->renewContext();
    JS_RunGC(jsRuntime_->getRuntime());
    js::setElementFinalizerShutdown(false);

    // ── Rebuild: the constructor's app-realm path, verbatim ──────────────────

    try {
        initAppRealm();
    } catch (const std::exception& e) {
        // index.html vanished or failed to parse mid-development. The engine
        // stays alive with no document (every consumer null-checks); a later
        // reload retries.
        LOG_ERROR("location.reload(): rebuild failed: %s", e.what());
    }

    uiDirty_ = true;
    systemDirty_ = true;   // menu bar reset above
    hasRenderedOnce_ = false;

    // Headless mirrors the constructor's initial flush so the reloaded app is
    // laid out + rasterizable before the driver's next evaluation.
    if (displayMode_ == DisplayMode::Headless) flush();
}

} // namespace bro::engine
