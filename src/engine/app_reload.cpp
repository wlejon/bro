#include "engine/engine.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/element_ref_adapter.h"
#include "render/skia_backend.h"
#if BRO_WITH_3D
#include "engine/gizmo.h"
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/log.h"

#include "bronze_host/bronze_host.h"
#include <exception>

namespace bro::engine {

void Engine::requestAppReload() {
    if (displayMode_ == DisplayMode::Server) return;
    pendingAppReload_ = true;
    uiDirty_ = true;
}

bool Engine::processPendingAppReload() {
    if (!pendingAppReload_) return false;
    pendingAppReload_ = false;
    performAppReload();
    return true;
}

void Engine::performAppReload() {
    LOG_INFO("Reloading app '%s'", appDir_.c_str());

    exitPointerLock();
    overlayMgr_.close();

    menuBar_.releaseHandlers();
    resetMenuBarDefaults();
    onMenuChanged();

#if BRO_WITH_3D
    if (gizmo_) gizmo_->clearCallbacks();
    clearSceneGraphs();
#endif

    inspector_.selected = nullptr;
    inspector_.pickerHover = nullptr;
    inspectorNodeMap_.clear();

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

    webglEntries_.clear();

    for (auto& d : iframeDocs_)
        if (d) queueIframeSurfaceFree(std::move(d->surface));
    destroyAllIframes();
    pendingIframeReloads_.clear();
    iframeLoadFailed_.clear();
    iframeSyncNeeded_ = false;

    destroyAllWindowHosts();
    if (displayMode_ == DisplayMode::Headless) {
        if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get()))
            drainIframeSurfaceFrees(skia);
    }

    layout::ElementRefAdapter::clearCache();
    transitionManager_.clearAll();
    animationManager_.clearAll();
    webAnimationManager_.clearAll();
    promotedElements_.clear();
    basePromotedSet_.clear();
    baseCommands_.clear();
    baseValid_ = false;
    appBaseDirty_ = true;
    lastLayoutContentW_ = lastLayoutContentH_ = -1;
    pointerCaptures_.clear();
    touchContacts_.clear();

    document_.reset();
    documentHeight_ = 0.0f;
    scrollY_ = 0.0f;
    wheelResidualY_ = 0.0f;
    selectionDragging_ = false;
    selectionPastThreshold_ = false;

    bro::bronze_host::clearHostTimers();
    bro::bronze_host::resetGlobalExpandos();

    try {
        initAppRealm();
    } catch (const std::exception& e) {
        LOG_ERROR("App reload failed: %s", e.what());
    }

    uiDirty_ = true;
    systemDirty_ = true;
    hasRenderedOnce_ = false;

    if (displayMode_ == DisplayMode::Headless) flush();
}

} // namespace bro::engine
