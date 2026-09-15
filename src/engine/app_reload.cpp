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
#include "bronze_host/app_module.h"
#include "bronze_host/host_gc.h"
#include "api/fs_watch.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>

namespace bro::engine {

namespace {

// Editors save in bursts (truncate, write, rename, attribute touch); the
// reload waits for this much quiet after the last change so one save is
// one reload.
constexpr double kWatchSettleMs = 150.0;

// A change under the app dir that means "the app's source changed": a code
// or markup file, not a save game, a settings file, an asset, or anything in
// a dot directory (.git, .bro_settings.json, editor swap files).
bool isWatchedSource(std::string_view relPath) {
    size_t start = 0;
    while (start < relPath.size()) {
        size_t slash = relPath.find('/', start);
        if (slash == std::string_view::npos) slash = relPath.size();
        if (slash > start && relPath[start] == '.') return false;
        start = slash + 1;
    }
    const size_t dot = relPath.rfind('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view ext = relPath.substr(dot);
    return ext == ".js" || ext == ".mjs" || ext == ".cjs" ||
           ext == ".html" || ext == ".htm" || ext == ".css";
}

int envSwitch(const char* name, const char* onValue, const char* offValue) {
    const char* v = std::getenv(name);
    if (!v) return -1;
    if (std::strcmp(v, onValue) == 0) return 1;
    if (std::strcmp(v, offValue) == 0) return 0;
    return -1;
}

} // namespace

void Engine::unloadAppModules() {
    for (uint64_t handle : appModuleHandles_) bro::bronze_host::unloadAppModule(handle);
    appModuleHandles_.clear();
}

void Engine::requestAppReload(AppReloadKind kind) {
    if (displayMode_ == DisplayMode::Server) return;
    // A Dev request outranks a Navigation one already pending: the source
    // changed, and the page's own reload would have compiled the old text.
    if (!pendingAppReload_ || kind == AppReloadKind::Dev) pendingAppReloadKind_ = kind;
    pendingAppReload_ = true;
    uiDirty_ = true;
}

bool Engine::processPendingAppReload() {
    if (!pendingAppReload_) return false;
    pendingAppReload_ = false;
    if (pendingAppReloadKind_ == AppReloadKind::Dev) devReloaded_ = true;
    performAppReload();
    return true;
}

bool Engine::jitOptimize() const {
    if (jitTierPin_ >= 0) return jitTierPin_ == 1;
    return !devReloaded_;
}

void Engine::initDevLoopConfig(const EngineConfig& config) {
    jitTierPin_ = envSwitch("BRO_JIT_TIER", "optimized", "baseline");
    watchSources_ = config.watchSources;
    if (const int pin = envSwitch("BRO_WATCH", "1", "0"); pin >= 0) watchSources_ = pin == 1;
}

void Engine::initAppWatcher() {
    appWatchers_.clear();
    appWatchPending_ = false;

    // The watchers are the edit loop's, so they exist only where there is an
    // edit loop: a window, and scripts the engine itself compiles from the
    // dir. A compiled app is rebuilt and relaunched by its own build; a
    // headless run is a test, whose driver owns every reload.
    if (!watchSources_ || displayMode_ != DisplayMode::Windowed || hostProvidesCompiledApp_ || appDir_.empty()) return;

    // The app dir, and the project's shared /lib when it is not already
    // inside it — the code an app imports from `/lib/...` is edited in the
    // same loop as its own.
    std::error_code ec;
    std::vector<std::string> dirs{std::filesystem::absolute(appDir_, ec).string()};
    if (auto lib = assetMounts_.mounts().find("/lib"); lib != assetMounts_.mounts().end()) {
        const auto rel = std::filesystem::relative(lib->second, dirs.front(), ec);
        const bool insideApp = !ec && !rel.empty() && rel.native()[0] != '.';
        if (!insideApp && std::filesystem::is_directory(lib->second, ec)) dirs.push_back(lib->second);
    }
    for (const auto& dir : dirs) {
        std::string err;
        auto watcher = brokit::api::FsWatcher::create(dir, /*recursive=*/true, &err);
        if (!watcher) {
            LOG_WARN("Not watching '%s' for source changes: %s", dir.c_str(), err.c_str());
            continue;
        }
        LOG_INFO("Watching '%s' for source changes (F5 reloads too)", dir.c_str());
        appWatchers_.push_back(std::move(watcher));
    }
}

void Engine::pollAppWatcher(double nowMs) {
    if (appWatchers_.empty()) return;
    std::vector<brokit::api::FsWatcher::Event> events;
    for (auto it = appWatchers_.begin(); it != appWatchers_.end();) {
        events.clear();
        (*it)->drain(events);
        bool failed = false;
        for (const auto& ev : events) {
            if (ev.type == brokit::api::FsWatcher::EventType::Error) {
                LOG_WARN("Source watcher on '%s' stopped: %s", (*it)->path().c_str(), ev.filename.c_str());
                failed = true;
                break;
            }
            if (!isWatchedSource(ev.filename)) continue;
            if (!appWatchPending_) LOG_INFO("Source changed: %s", ev.filename.c_str());
            appWatchPending_ = true;
            appWatchLastChangeMs_ = nowMs;
        }
        it = failed ? appWatchers_.erase(it) : it + 1;
    }
    if (appWatchPending_ && nowMs - appWatchLastChangeMs_ >= kWatchSettleMs) {
        appWatchPending_ = false;
        requestAppReload(AppReloadKind::Dev);
    }
}

void Engine::performAppReload() {
    LOG_INFO("Reloading app '%s' (%s)", appDir_.c_str(),
             jitOptimize() ? "optimized" : "baseline tier");

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
    bro::bronze_host::clearHostMediaQueries();

    if (!appModuleHandles_.empty()) {
        unloadAppModules();
        bro::bronze_host::hostCollectGarbage();
    }

    try {
        initAppRealm();
        if (hostProvidesCompiledApp_) {
            if (auto modulePath = bro::bronze_host::findAppModule(appDir_)) {
                bro::bronze_host::runAppModule(*this, *modulePath);
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("App reload failed: %s", e.what());
    }

    uiDirty_ = true;
    systemDirty_ = true;
    hasRenderedOnce_ = false;

    if (displayMode_ == DisplayMode::Headless) flush();
}

} // namespace bro::engine
