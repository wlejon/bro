#include "engine/engine.h"
#include "engine/source_preflight.h"

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
#include "util/time.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/app_module.h"
#include "bronze_host/host_gc.h"
#include "api/fs_watch.h"
#if BRO_WITH_COMPOSITOR
#include <brocompositor/api.h>
#endif

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string_view>

namespace bro::engine {

namespace {

// Editors save in bursts (truncate, write, rename, attribute touch), and a
// git checkout or a build step that copies a UI into place writes a whole
// tree; the reload waits for this much quiet after the last change so one
// save, or one checkout, is one reload. Quiet is not proof the tree is whole,
// so the reload is preflighted too (pollAppWatcher).
constexpr double kWatchSettleMs = 300.0;

// The page text a reload leaves up when the new one could not be loaded at
// all: the error, in place of a blank window.
std::string reloadErrorPage(const std::string& what) {
    std::string esc;
    for (char c : what) {
        if (c == '<') esc += "&lt;";
        else if (c == '>') esc += "&gt;";
        else if (c == '&') esc += "&amp;";
        else esc += c;
    }
    return "<!DOCTYPE html><html><head><title>Reload failed</title></head>"
           "<body style=\"margin:0;background:#1b1b1f;color:#eee;font:14px sans-serif\">"
           "<div id=\"__bro_reload_error\" style=\"padding:16px 20px\">"
           "<div style=\"font-weight:bold;color:#ff6b6b;margin-bottom:8px\">App reload failed</div>"
           "<pre style=\"white-space:pre-wrap;margin:0\">" + esc + "</pre>"
           "<div style=\"margin-top:12px;color:#aaa\">Fix the source and save to reload, or press F5.</div>"
           "</div></body></html>";
}

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

void Engine::initDevLoopConfig(const EngineConfig& config) {
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
    if (!watchSources_ || (displayMode_ != DisplayMode::Windowed && displayMode_ != DisplayMode::Drm) || hostProvidesCompiledApp_ || appDir_.empty()) return;

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
    if (appWatchers_.empty() && !appWatchRearm_) return;
    std::vector<brokit::api::FsWatcher::Event> events;
    auto markChanged = [&](const std::string& what) {
        if (!appWatchPending_) LOG_INFO("Source changed: %s", what.c_str());
        appWatchPending_ = true;
        appWatchLastChangeMs_ = nowMs;
    };
    for (auto it = appWatchers_.begin(); it != appWatchers_.end();) {
        events.clear();
        (*it)->drain(events);
        bool failed = false;
        for (const auto& ev : events) {
            if (ev.type == brokit::api::FsWatcher::EventType::Error) {
                // A burst bigger than the watcher's ring (a checkout of the
                // whole tree) drops events but leaves the watcher running:
                // that is a change of unknown files, not a dead watcher.
                if (ev.filename.rfind("fs.watch ring overflow", 0) == 0) {
                    markChanged("(many files)");
                    continue;
                }
                LOG_WARN("Source watcher on '%s' stopped: %s (re-watching)", (*it)->path().c_str(),
                         ev.filename.c_str());
                failed = true;
                break;
            }
            if (ev.filename.empty()) {
                // The watched dir itself was removed or moved away (a deploy
                // step that replaces the folder): this watcher is done, and a
                // fresh one goes on the folder once it is back.
                failed = true;
                markChanged("(the app folder was replaced)");
                break;
            }
            if (!isWatchedSource(ev.filename)) continue;
            markChanged(ev.filename);
        }
        if (failed) {
            appWatchRearm_ = true;
            it = appWatchers_.erase(it);
        } else {
            ++it;
        }
    }
    if (appWatchRearm_ && nowMs - appWatchLastChangeMs_ >= kWatchSettleMs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(appDir_, ec)) return;   // not back yet
        const bool pending = appWatchPending_;
        appWatchRearm_ = false;
        initAppWatcher();
        appWatchPending_ = pending;
        if (appWatchers_.empty()) return;
    }
    if (appWatchPending_ && nowMs - appWatchLastChangeMs_ >= kWatchSettleMs) {
        appWatchPending_ = false;
        // A reload throws the running page away, so it only goes ahead on a
        // tree that is whole: every file index.html and its module graph name
        // is there and exports what is imported from it. A tree caught mid-
        // checkout (or genuinely broken) keeps the running page, and the next
        // change looks again; F5 reloads regardless.
        std::string why = checkAppSourceConsistency(appDir_, &assetMounts_);
        if (!why.empty()) {
            if (why != appWatchDeferReason_)
                LOG_WARN("Not reloading yet, the source looks incomplete: %s "
                         "(keeping the running page; the next change checks again, F5 reloads anyway)",
                         why.c_str());
            appWatchDeferReason_ = std::move(why);
            return;
        }
        appWatchDeferReason_.clear();
        requestAppReload();
    }
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

    if (document_) {
        document_->forEachLiveElement(
            [](dom::Element* el) { el->setCanvasScene(nullptr); });
    }
    canvasScenesDetached_.clear();
    canvasScenes_.clear();
    canvasSceneRegistry_.clear();

    webglEntries_.clear();

    for (auto& d : iframeDocs_)
        if (d) {
            queueIframeSurfaceFree(std::move(d->surface));
            queueIframeSurfaceFree(std::move(d->spare));
        }
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
    setViewportScrollY(0.0f);   // document_ is gone; nothing to mirror into
    wheelResidualY_ = 0.0f;
    selectionDragging_ = false;
    selectionPastThreshold_ = false;

    bro::bronze_host::clearHostTimers();
    bro::bronze_host::resetGlobalExpandos();
    bro::bronze_host::clearHostMediaQueries();
    bro::bronze_host::clearHostCanvases();
    bro::bronze_host::clearAllElementListeners();
    bro::bronze_host::resetCustomElementsRegistry();
    bro::bronze_host::clearParsedDocuments();
    bro::bronze_host::clearDynamicModules();
    bro::bronze_host::clearMenuHandlers();
#if BRO_WITH_COMPOSITOR
    // The compositor outlives the page (a shell host reloading its shell):
    // the old page's listeners and edge reservations go with it, or the new
    // page's bar would be reserved on top of the old one's.
    brocompositor::api::resetCompositorScript();
#endif

    // The registry of module namespaces the page published goes with it.
    // Kept, it would make the new page's compile bind every file the old one
    // imported as an external instance: edits to those modules would never
    // load, and a renamed export would read undefined.
    bro::bronze_host::clearAppModuleRegistry();
    if (!appModuleHandles_.empty()) {
        unloadAppModules();
        bro::bronze_host::hostCollectGarbage();
    }

    if (displayMode_ == DisplayMode::Windowed && splashEnabled_) {
        splashVisible_ = true;
        splashDismissTriggered_ = false;
        splashStartMs_ = util::currentTimeMs();
        pumpSplashFrame(0.0);
    }

    // The failure flag reports this reload's own scripts; a failure already
    // latched (a headless test's) is put back afterwards.
    const bool failedBefore = hasTestFailure();
    clearTestFailure();
    std::string failure;
    try {
        initAppRealm();
        if (hostProvidesCompiledApp_) {
            if (auto modulePath = bro::bronze_host::findAppModule(appDir_)) {
                bro::bronze_host::runAppModule(*this, *modulePath);
            }
        }
        if (hasTestFailure())
            failure = "A script of the reloaded page threw at its top level (the error is logged above).";
    } catch (const std::exception& e) {
        failure = e.what();
        if (!document_) {
            // Nothing parsed: put up a page saying so rather than a blank one.
            appHtmlOverride_ = reloadErrorPage(failure);
            try { initAppRealm(); } catch (const std::exception&) {}
            appHtmlOverride_.clear();
        }
    }
    if (!failure.empty()) showAppReloadFailure(failure);
    if (failedBefore) setTestFailure(true);

    uiDirty_ = true;
    systemDirty_ = true;
    hasRenderedOnce_ = false;

    if (displayMode_ == DisplayMode::Headless) flush();
}

void Engine::showAppReloadFailure(const std::string& what) {
    LOG_ERROR("App reload failed: %s", what.c_str());
    // The banner is the edit loop's: a headless test asserts on the page it
    // reloaded, not on an overlay the engine added to it.
    if (displayMode_ != DisplayMode::Windowed && displayMode_ != DisplayMode::Drm) return;
    if (!document_ || !document_->body()) return;
    if (document_->getElementById("__bro_reload_error")) return;   // the error page itself
    dom::Element* banner = document_->createElement("div");
    banner->setAttribute("id", "__bro_reload_error");
    banner->setAttribute("style",
                         "position:fixed;left:0;right:0;top:0;z-index:2147483647;"
                         "background:#b00020;color:#fff;font:13px monospace;"
                         "padding:6px 10px;white-space:pre-wrap");
    banner->setTextContent("App reload failed: " + what +
                           " The previous page could not be kept. Fix the source and save, or press F5.");
    document_->body()->appendChild(banner);
    document_->markDirty();
}

} // namespace bro::engine
