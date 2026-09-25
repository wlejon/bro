#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/scene_audio_sync.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/app_module.h"
#include "bronze_host/host_gc.h"
#include "api/fs_watch.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
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
#include "platform/dialogs.h"
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
    bro::bronze_host::terminateAllWorkers();
#if BRO_WITH_NET
    netService_.reset();
#endif
    steamService_.reset();
}

void Engine::shutdown() {
    if (shutdownDone_) return;
    shutdownDone_ = true;

    util::beginShutdown();

    // Sibling async registries first: they join work threads that may still
    // be driving brotensor and hold rooted JS callbacks.
    for (auto& hook : shutdownHooks_) hook();
    shutdownHooks_.clear();
    framePumps_.clear();

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

    if (canvasRasterThread_ && canvasRasterThread_->started()) {
        for (auto& cs : canvasScenes_) {
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        }
        for (auto& cs : canvasScenesDetached_) {
            if (cs && cs->isThreaded()) canvasRasterThread_->releaseScene(cs.get());
        }
        canvasRasterThread_->stop();
    }

    closeAllGamepads();
    destroyAllWindowHosts();
    removeModalEventWatch();
}

Engine::~Engine() {
    // The dialog tick callback captures this engine.
    platform::Dialogs::setTickCallback(nullptr);
    platform::Dialogs::setWindow(nullptr);

    appWatchers_.clear();

    shutdown();

#if BRO_WITH_TENSOR
    brotensor::shutdown();
#endif

    if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        for (auto& ps : screenshotHtmlPool_) skia->destroyGPUSurface(ps);
        screenshotHtmlPool_.clear();
        for (auto& ps : screenshotSystemPool_) skia->destroyGPUSurface(ps);
        screenshotSystemPool_.clear();

        for (auto& d : iframeDocs_) {
            if (!d) continue;
            skia->destroyGPUSurface(d->surface);
            d->surfW = d->surfH = 0;
            d->fboTexture = 0;
        }
        drainIframeSurfaceFrees(skia);
    }

    menuBar_.releaseHandlers();

#if BRO_WITH_3D
    clearSceneGraphs();
#endif

    if (document_) {
        document_->forEachLiveElement(
            [](dom::Element* el) { el->setCanvasScene(nullptr); });
    }
    canvasScenesDetached_.clear();
    canvasScenes_.clear();
    canvasSceneRegistry_.clear();
    canvasRasterThread_.reset();

    webglEntries_.clear();
    destroyAllIframes();
    destroySystemPanels();

    layout::ElementRefAdapter::clearCache();

    if (audioInference_) audioInference_->shutdown();

    stopBackgroundServices();

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

    drawTraversal_.reset();

#if BRO_WITH_3D
    if (gizmo_) gizmo_->clearCallbacks();
    clearSceneGraphs();
#endif

#if BRO_WITH_PHYSICS
    physicsWorld_.reset();
#endif
    audioInference_.reset();
    SceneAudioSync::shutdown();
    bro::bronze_host::hostCollectGarbage();
    document_.reset();
    audioEngine_.reset();
    renderer_.reset();

    unloadAppModules();
    bro::bronze_host::hostCollectGarbage();
}

void Engine::handleResize(int w, int h) {
    // A script reaches this with its own numbers (headless resize(), a
    // headless bro.window.setSize): the viewport sizes every layer surface
    // and readback, so it stays within what one GL texture can be. An OS
    // window never reports a size outside this.
    constexpr int kMaxViewportSide = 16384;
    w = std::clamp(w, 1, kMaxViewportSide);
    h = std::clamp(h, 1, kMaxViewportSide);
    viewportWidth_ = w;
    viewportHeight_ = h;
    updateDeviceScale();
    applyMediaResolution();
    uiDirty_ = true;
    hasRenderedOnce_ = false;
    drawTraversal_->setViewport(contentWidth(), contentHeight(), 0);
    resizeSystemPanels(w, h);

    int cw = contentWidth();
    int ch = contentHeight();
    if (document_) {
        if (document_->isStructureDirty())
            ensureReplacedElements(document_->documentElement());
        document_->setMediaViewport(static_cast<float>(cw), static_cast<float>(ch));
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_.get());
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(cw), static_cast<float>(ch), *textMetrics_);
        updateDocumentHeight();
        float maxScroll = std::max(0.0f, documentHeight_ - static_cast<float>(ch));
        setViewportScrollY(std::clamp(scrollY_, 0.0f, maxScroll));

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

        dom::Event resizeEvt("resize", /*bubbles=*/false, /*cancelable=*/false);
        resizeEvt.setIsTrusted(true);
        dom::dispatchWindowEvent(document_.get(), resizeEvt);
    }

    deliverMediaQueryChangesAllRealms();
}

void Engine::handleDisplayScaleChanged() {
    if (displayMode_ != DisplayMode::Windowed || !window_) return;
    const int oldW = deviceScale_.drawableW, oldH = deviceScale_.drawableH;
    // A new scale re-rasterizes everything at it and tells the realms: the
    // resize path does both (resize event, matchMedia change listeners).
    if (updateDeviceScale()) {
        LOG_INFO("Device scale now %.2f (devicePixelRatio %.2f), drawable %dx%d",
                 deviceScale_.render, deviceScale_.ratio,
                 deviceScale_.drawableW, deviceScale_.drawableH);
        handleResize(viewportWidth_, viewportHeight_);
    } else if (deviceScale_.drawableW != oldW || deviceScale_.drawableH != oldH) {
        uiDirty_ = true;
    }
}

bool Engine::updateDeviceScale() {
    const DeviceScale prev = deviceScale_;
    if (displayMode_ == DisplayMode::Windowed && window_) {
        deviceScale_.render = window_->getPixelDensity();
        deviceScale_.ratio = window_->getDevicePixelRatio();
    } else {
        // Headless follows the configured factor. The CPU fallback (no GL)
        // rasterizes 1:1 but still reports the configured ratio.
        deviceScale_.ratio = deviceScale_.configured;
        deviceScale_.render = gl_ ? deviceScale_.configured : 1.0f;
    }
    int pw = 0, ph = 0;
    if (displayMode_ == DisplayMode::Windowed && window_)
        window_->getSizeInPixels(pw, ph);
    deviceScale_.drawableW = pw > 0 ? pw : deviceScale_.toDevice(viewportWidth_);
    deviceScale_.drawableH = ph > 0 ? ph : deviceScale_.toDevice(viewportHeight_);
    return deviceScale_.render != prev.render || deviceScale_.ratio != prev.ratio;
}

void Engine::setDeviceScaleFactor(float scale) {
    deviceScale_.configured = std::clamp(scale, 0.25f, 8.0f);
    if (updateDeviceScale()) handleResize(viewportWidth_, viewportHeight_);
}

void Engine::applyMediaResolution() {
    const float dppx = deviceScale_.ratio;
    if (document_) document_->setMediaResolution(dppx);
    for (auto& doc : iframeDocs_) {
        if (doc && doc->document) doc->document->setMediaResolution(dppx);
    }
    for (auto& doc : systemDocs_) {
        if (doc.document) doc.document->setMediaResolution(dppx);
    }
    for (auto& host : windowHosts_) {
        if (host && host->document)
            host->document->setMediaResolution(static_cast<float>(host->displayScale));
    }
}

std::string Engine::effectiveColorScheme() const {
    if (settings_) {
        const std::string& pref = settings_->appearance().colorScheme;
        if (pref == "light" || pref == "dark") return pref;
    }
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
    for (auto& host : windowHosts_) {
        if (host && host->document) host->document->setMediaColorScheme(scheme);
    }
}

void Engine::deliverMediaQueryChangesAllRealms() {
    if (document_ && document_->mediaRestylePending()) document_->resolveStyles();
    for (auto& d : iframeDocs_) {
        if (d && d->document && d->document->mediaRestylePending()) d->document->resolveStyles();
    }
    for (auto& h : windowHosts_) {
        if (h && h->document && h->document->mediaRestylePending()) h->document->resolveStyles();
    }
    bro::bronze_host::deliverHostMediaQueryChanges();
}

} // namespace bro::engine
