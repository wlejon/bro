#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/inspector_highlight.h"
#include "engine/key_mapping.h"
#include "dom/element_geometry.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "engine/default_styles.h"
#include "engine/scene_audio_sync.h"
#include "dom/event_dispatch.h"
#include "util/asset_path.h"
#include "util/user_dirs.h"

#include <filesystem>
#include <fstream>

#include "platform/sdl_window.h"
#include "platform/dialogs.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "render/bidi.h"

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "bronze_host/eval.h"
#include "audio_inference/audio_inference.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#include "steam/steam_service.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include <broaudio/engine.h>
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "util/log.h"
#include "util/time.h"

#include <glad/gl.h>
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace bro::engine {
Engine::Engine(const EngineConfig& config)
    : graphicsConfig_(config.graphics)
    , inputConfig_(config.input)
    , displayMode_(config.displayMode)
    , viewportWidth_(config.graphics.width)
    , viewportHeight_(config.graphics.height)
    , viewportScrollbar_(config.viewportScrollbar)
    , elementScrollbar_(config.elementScrollbar)
    , uiFrameIntervalMs_(config.graphics.maxFrameIntervalMs) {

    splashEnabled_ = config.showSplash;
    compiledApp_ = config.compiledApp;
    hostProvidesCompiledApp_ = config.hostProvidesCompiledApp;
    appDir_ = config.appDir;
    titleOverride_ = config.title;

    // === Asset mounts (engine-supplied virtual paths: /lib, /system, ...) ===
    {
        namespace fs = std::filesystem;
        auto tryMount = [&](const std::string& prefix, const std::string& dirName) {
            if (!config.appDir.empty()) {
                fs::path appLocal = fs::path(config.appDir) / dirName;
                std::error_code ec;
                if (fs::is_directory(appLocal, ec)) {
                    assetMounts_.addMount("/" + prefix, fs::absolute(appLocal, ec).string());
                    return;
                }
            }
            if (!config.projectRoot.empty()) {
                fs::path rootLocal = fs::path(config.projectRoot) / dirName;
                std::error_code ec;
                if (fs::is_directory(rootLocal, ec)) {
                    assetMounts_.addMount("/" + prefix, fs::absolute(rootLocal, ec).string());
                }
            }
        };
        tryMount("lib",    config.libDirName.empty()    ? "lib"    : config.libDirName);
        tryMount("system", config.systemDirName.empty() ? "system" : config.systemDirName);
        tryMount("std", "std");
        if (!config.appDir.empty()) {
            std::error_code ec;
            assetMounts_.addMount("/app", fs::absolute(config.appDir, ec).string());
        }
    }

    // === Settings system ===
    settings_ = std::make_unique<Settings>(config.settingsPath);
    settings_->defineEngineAction("system_toggle_perf", {"F8"});
    settings_->defineEngineAction("system_toggle_settings", {});
    settings_->applyAppOverrides(config.graphics, config.input);

    auto& gfx = settings_->graphics();
    auto& inp = settings_->input();
    viewportWidth_ = gfx.width;
    viewportHeight_ = gfx.height;
    uiFrameIntervalMs_ = gfx.maxFrameIntervalMs;
    frameCapIntervalMs_ = gfx.maxFps > 0.0 ? 1000.0 / gfx.maxFps : 0.0;
    inputConfig_.scrollSpeed = inp.scrollSpeed;
    inputConfig_.doubleClickThresholdMs = inp.doubleClickThresholdMs;
    inputConfig_.doubleClickDistancePx = inp.doubleClickDistancePx;
    inputConfig_.overlayToggleKey = inp.overlayToggleKey;

    engineNowMs_ = displayMode_ == DisplayMode::Headless ? virtualTime_
                                                         : util::currentTimeMs();

#if BRO_WITH_PHYSICS
    physicsWorld_ = std::make_unique<physics::PhysicsWorld>();
    physicsWorld_->init();
    if (displayMode_ != DisplayMode::Headless)
        physicsWorld_->startThread();
#endif

#if BRO_WITH_NET
    netService_ = std::make_unique<net::NetService>();
#endif

    steamService_ = std::make_unique<steam::SteamService>();
    serverStartTime_ = util::currentTimeMs();

    // Server mode: lightweight init — no rendering, DOM, or audio
    if (displayMode_ == DisplayMode::Server) {
        LOG_INFO("Server mode initialized (no rendering, no DOM, no audio)");
        return;
    }

    // Windowed / Headless initialization (rendering + DOM)
    const bool hasGL = (displayMode_ == DisplayMode::Windowed) || config.graphics.useGPU;

    if (hasGL) {
        bool hidden = (displayMode_ == DisplayMode::Headless);
        try {
            window_ = std::make_unique<platform::Window>("Bro",
                static_cast<uint32_t>(gfx.width),
                static_cast<uint32_t>(gfx.height), hidden,
                gfx.resizable, gfx.vsync, config.graphics.borderless);

            const auto& wcfg = config.graphics;
            if (wcfg.alwaysOnTop) window_->setAlwaysOnTop(true);
            if (wcfg.minWidth > 0 || wcfg.minHeight > 0)
                window_->setMinimumSize(wcfg.minWidth, wcfg.minHeight);
            if (wcfg.maxWidth > 0 || wcfg.maxHeight > 0)
                window_->setMaximumSize(wcfg.maxWidth, wcfg.maxHeight);
            if (!hidden) {
                if (wcfg.display >= 0) {
                    auto displays = window_->getDisplays();
                    if (wcfg.display < static_cast<int>(displays.size())) {
                        window_->moveToDisplay(displays[wcfg.display].id);
                    } else {
                        LOG_WARN("bro.json display=%d, but only %zu display(s) attached",
                                 wcfg.display, displays.size());
                    }
                }
                if (wcfg.windowX != kWindowPosUnset && wcfg.windowY != kWindowPosUnset)
                    window_->setPosition(wcfg.windowX, wcfg.windowY);
            }

            if (!hidden) {
                window_->setIcon("system/icon.png");
                displayScale_ = window_->getDisplayScale();
            }

            gl_ = std::make_unique<render::GLContext>(*window_);
            renderer_ = render::createRenderer(gl_.get());
            if (!renderer_) {
                throw std::runtime_error("Failed to create renderer");
            }
        } catch (const std::exception& e) {
            if (displayMode_ == DisplayMode::Headless) {
                LOG_WARN("GPU init failed (%s); falling back to CPU raster rendering", e.what());
                gl_.reset();
                window_.reset();
                renderer_ = std::make_unique<render::RasterRenderer>();
            } else {
                throw;
            }
        }
    } else {
        renderer_ = std::make_unique<render::RasterRenderer>();
    }

    if (displayMode_ == DisplayMode::Headless) {
        virtualTime_ = util::currentTimeMs();
        engineNowMs_ = virtualTime_;
    }

    audioEngine_ = std::make_unique<broaudio::Engine>();
    if (displayMode_ == DisplayMode::Windowed || config.realAudio) {
        audioEngine_->init();
    } else {
        audioEngine_->initHeadless();
    }

    SceneAudioSync::install(audioEngine_.get());

    audioInference_ = std::make_unique<AudioInference>();
    if (displayMode_ != DisplayMode::Headless)
        audioInference_->startThread();

    {
        auto& audio = settings_->audio();
        float vol = audio.muted ? 0.0f : audio.masterVolume;
        audioEngine_->setMasterGain(vol);
    }

    settings_->setChangeCallback([this](const std::string& category,
                                        const std::string& key) {
        if (category == "graphics" || category == "*") {
            auto& g = settings_->graphics();
            if ((key == "fullscreen" || key == "*") && window_) {
                window_->setFullscreen(g.fullscreen);
                setFullscreenState(g.fullscreen);
            }
            if ((key == "vsync" || key == "*") && window_)
                window_->setVSync(g.vsync);
            if ((key == "width" || key == "height" || key == "*") && window_ && !g.fullscreen)
                window_->setWindowSize(static_cast<uint32_t>(g.width),
                                       static_cast<uint32_t>(g.height));
            if ((key == "resizable" || key == "*") && window_)
                window_->setResizable(g.resizable);
            if (key == "maxFrameIntervalMs" || key == "*")
                uiFrameIntervalMs_ = g.maxFrameIntervalMs;
            if (key == "maxFps" || key == "*")
                frameCapIntervalMs_ = g.maxFps > 0.0 ? 1000.0 / g.maxFps : 0.0;
        }
        if (category == "audio" || category == "*") {
            auto& a = settings_->audio();
            float vol = a.muted ? 0.0f : a.masterVolume;
            audioEngine_->setMasterGain(vol);
        }
        if (category == "input" || category == "*") {
            auto& inpSettings = settings_->input();
            inputConfig_.scrollSpeed = inpSettings.scrollSpeed;
            inputConfig_.doubleClickThresholdMs = inpSettings.doubleClickThresholdMs;
            inputConfig_.doubleClickDistancePx = inpSettings.doubleClickDistancePx;
            inputConfig_.overlayToggleKey = inpSettings.overlayToggleKey;
        }
        if (category == "appearance" || category == "*") {
            applyColorScheme();
        }
        // Last, so an observer reads the engine's post-change state.
        if (settingsObserver_) settingsObserver_(category, key);
    });

    resetMenuBarDefaults();

#if BRO_WITH_3D
    gizmo_ = std::make_unique<GizmoManager>();
#endif

    recordingRenderer_ = std::make_unique<render::RecordingRenderer>(nullptr, renderer_.get());
    drawTraversal_ = std::make_unique<layout::DrawTraversal>(recordingRenderer_.get());
    textMetrics_ = std::make_unique<layout::SkiaTextMetrics>(renderer_.get());

    if (displayMode_ == DisplayMode::Windowed) {
        eventLoop_ = std::make_unique<platform::EventLoop>();
    }

    // Native dialogs parent on the window and tick timers while they are up.
    // Only a windowed run has someone to answer them; headless and server
    // answer themselves (see platform::Dialogs) instead of blocking on a
    // window nobody sees. Set before any script runs: the first alert() an
    // app's boot script reaches must already know there is no one to ask.
    platform::Dialogs::setWindow(window_ ? window_->getSDLWindow() : nullptr);
    platform::Dialogs::setInteractive(displayMode_ == DisplayMode::Windowed);
    platform::Dialogs::setTickCallback([this]() { tickTimersOnly(); });

    if (gl_) {
        glGenVertexArrays(1, &uiQuadVAO_);
        glGenBuffers(1, &uiQuadVBO_);
    }

    manifest_ = AppLoader::loadApp(appDir_, &assetMounts_);
    util::setAssetPathContext(manifest_.basePath, &assetMounts_);
    drawTraversal_->setViewport(viewportWidth_, viewportHeight_, 0);

    initSystemPanels();

    if (displayMode_ != DisplayMode::Server && splashEnabled_) {
        for (auto& d : systemDocs_) {
            if (d.group == "splash") {
                splashVisible_ = true;
                splashStartMs_ = (displayMode_ == DisplayMode::Headless)
                    ? virtualTime_
                    : util::currentTimeMs();
                break;
            }
        }
    }

    if (displayMode_ == DisplayMode::Windowed && splashVisible_ && window_ && renderer_) {
        fireFrameCallbacks(0.0);
        tickSystemPanels(splashStartMs_);
        stageSystemPanelCanvases();
        renderSplashImmediate();
    }

    initAppRealm();

    if (displayMode_ == DisplayMode::Headless) {
        flush();
    }
}

void Engine::initAppRealm() {
    documentReadyState_ = "loading";

    if (compiledApp_ && !hostProvidesCompiledApp_) {
        LOG_WARN("App '%s' declares \"compiled\": true, but host provides no compiled app module.",
                 appDir_.c_str());
    }

    manifest_ = AppLoader::loadApp(appDir_, &assetMounts_);
    util::setAssetPathContext(manifest_.basePath, &assetMounts_);
    std::string html = AppLoader::loadFile(manifest_.htmlPath);
    if (html.empty()) {
        throw std::runtime_error("Failed to load index.html from " + appDir_);
    }

    drawTraversal_->setBasePath(manifest_.basePath);
    drawTraversal_->setViewport(contentWidth(), contentHeight(), 0);

    std::string authorStyles;
    for (auto& cssPath : manifest_.stylePaths) {
        std::string css = AppLoader::loadFile(cssPath);
        if (!css.empty()) {
            authorStyles += css + "\n";
        }
    }

    std::vector<dom::Document::TemplateBlock> templateBlocks;
    html = dom::Document::extractTemplates(html, templateBlocks);

    document_ = std::make_unique<dom::Document>();
    document_->setBasePath(manifest_.basePath);
    document_->setMediaViewport(static_cast<float>(contentWidth()),
                                static_cast<float>(contentHeight()));
    document_->setMediaColorScheme(effectiveColorScheme());
    document_->cascade().setImportResolver([this](const std::string& url) {
        std::string path = AppLoader::resolvePath(document_->basePath(), url,
                                                  &assetMounts_);
        std::string css = AppLoader::loadFile(path);
        if (css.empty()) {
            LOG_WARN("@import: failed to load '%s' (resolved to '%s')",
                     url.c_str(), path.c_str());
        }
        return css;
    });
    document_->parse(html, authorStyles, kDefaultStyles);

    if (!templateBlocks.empty())
        document_->injectTemplates(templateBlocks);

    if (window_) {
        if (!titleOverride_.empty()) {
            window_->setTitle(titleOverride_);
        } else {
            std::string docTitle = document_->title();
            if (!docTitle.empty()) {
                window_->setTitle(docTitle);
            }
        }
    }

    loadCustomFonts();
    if (document_) {
        ensureReplacedElements(document_->documentElement());
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_.get());
        document_->setTransitionManager(&transitionManager_, engineNowMs_);
        animationManager_.setKeyframes(&document_->cascade().keyframes());
        document_->setAnimationManager(&animationManager_);
        document_->setWebAnimationManager(&webAnimationManager_);
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(viewportWidth_),
                                 static_cast<float>(contentHeight()), *textMetrics_);
        if (document_->documentElement()) {
            auto& box = document_->documentElement()->layoutBox();
            documentHeight_ = box.marginBox().height;
        }
        syncIframes();
    }

    if (!manifest_.scripts.empty()) {
        std::string combinedScripts;
        for (const auto& script : manifest_.scripts) {
            std::string code;
            if (script.isInline()) {
                code = script.code;
            } else {
                code = AppLoader::loadFile(script.path);
            }
            if (!code.empty()) {
                if (!combinedScripts.empty()) combinedScripts += "\n;\n";
                combinedScripts += code;
            }
        }
        if (!combinedScripts.empty()) {
            if (!bro::bronze_host::evalScript(*this, combinedScripts, manifest_.htmlPath)) {
                setTestFailure(true);
            }
        }
    }

    documentReadyState_ = "interactive";
    if (auto* root = document_ ? document_->documentElement() : nullptr) {
        bro::dom::Event dclDom("DOMContentLoaded", /*bubbles=*/true, /*cancelable=*/false);
        dom::dispatchDomEvent(root, dclDom);
    }
    documentReadyState_ = "complete";
    if (auto* root = document_ ? document_->documentElement() : nullptr) {
        bro::dom::Event loadDom("load", /*bubbles=*/false, /*cancelable=*/false);
        dom::dispatchDomEvent(root, loadDom);
    }
    if (document_) {
        bro::dom::Event loadWin("load", /*bubbles=*/false, /*cancelable=*/false);
        dispatchWindowEvent(loadWin);
    }

    mediaEventsArmed_ = true;
}

webgl::WebGL2RenderingContext* Engine::createWebGL2Context(dom::Element* canvas) {
    if (!gl_) return nullptr;

    if (canvas && canvas->webglContext()) {
        return static_cast<webgl::WebGL2RenderingContext*>(canvas->webglContext());
    }

    int cw = viewportWidth_, ch = viewportHeight_;
    if (canvas) {
        auto& box = canvas->layoutBox();
        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
    }
    auto ctx2 = std::make_unique<webgl::WebGL2RenderingContext>(cw, ch);
    auto* webglCtx = ctx2.get();
    if (canvas) canvas->setWebglContext(webglCtx);
    webglEntries_.push_back({std::move(ctx2), canvas});
    return webglCtx;
}

canvas::CanvasScene* Engine::createCanvasContext(dom::Element* canvas) {
    if (!canvas) return nullptr;
    if (canvas->canvasScene()) {
        return static_cast<canvas::CanvasScene*>(canvas->canvasScene());
    }

    auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
    int w = 300, h = 150;
    const std::string wAttr = canvas->getAttribute("width");
    const std::string hAttr = canvas->getAttribute("height");
    if (!wAttr.empty()) w = std::atoi(wAttr.c_str());
    if (!hAttr.empty()) h = std::atoi(hAttr.c_str());
    canvasScene->setIntrinsicWidth(w);
    canvasScene->setIntrinsicHeight(h);
    canvasScene->ensureSurface(w, h);
    canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
        auto* elem = static_cast<dom::Element*>(ud);
        if (!elem->parentNode()) {
            ox = oy = ow = oh = 0;
            return;
        }
        dom::AbsoluteRect r = dom::absoluteContentBox(elem);
        ox = r.x; oy = r.y; ow = r.width; oh = r.height;
    }, canvas);
    canvasScene->setDetachedCallback([](void* ud) -> bool {
        auto* n = static_cast<dom::Element*>(ud);
        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
        return n->tagName() != "html" && n->tagName() != "HTML";
    }, canvas);
    canvasScene->setLiveCheck([](void* doc, void* node) -> bool {
        return static_cast<dom::Document*>(doc)->isNodeLive(
            static_cast<dom::Element*>(node));
    }, canvas->document());

    auto* csPtr = canvasScene.get();
    canvas->setCanvasScene(csPtr, &canvas::CanvasScene::onBackingElementDestroyed);
    canvasScene->init(nullptr);
    canvasSceneRegistry_[canvasScene->sceneId()] = csPtr;
    dom::Document* doc = canvas->document();
    if (doc) {
        for (auto& h : windowHosts_) {
            if (h && h->document.get() == doc) {
                h->canvasScenes.push_back(std::move(canvasScene));
                return csPtr;
            }
        }
        for (auto& d : iframeDocs_) {
            if (d && d->document.get() == doc) {
                d->canvasScenes.push_back(std::move(canvasScene));
                return csPtr;
            }
        }
        for (auto& s : systemDocs_) {
            if (s.document.get() == doc) {
                s.canvasScenes.push_back(std::move(canvasScene));
                return csPtr;
            }
        }
    }
    canvasScenes_.push_back(std::move(canvasScene));
    return csPtr;
}

scene::SceneGraph* Engine::createSceneContext(dom::Element* canvas) {
#if !BRO_WITH_3D
    (void)canvas;
    return nullptr;
#else
    if (!gl_) return nullptr;

    if (canvas && canvas->sceneGraph()) {
        if (auto* existing = sceneGraphForElement(canvas)) return existing;
    }
    if (!canvas) return nullptr;

    auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
    canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
        auto* elem = static_cast<dom::Element*>(ud);
        if (!elem->parentNode()) {
            ox = oy = ow = oh = 0;
            return;
        }
        dom::AbsoluteRect r = dom::absoluteContentBox(elem);
        ox = r.x; oy = r.y; ow = r.width; oh = r.height;
    }, canvas);
    canvasScene->setDetachedCallback([](void* ud) -> bool {
        auto* n = static_cast<dom::Element*>(ud);
        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
        return n->tagName() != "html" && n->tagName() != "HTML";
    }, canvas);
    canvasScene->setLiveCheck([](void* doc, void* node) -> bool {
        return static_cast<dom::Document*>(doc)->isNodeLive(
            static_cast<dom::Element*>(node));
    }, canvas->document());

    auto* csPtr = canvasScene.get();
    canvas->setCanvasScene(csPtr, &canvas::CanvasScene::onBackingElementDestroyed);
    addCanvasScene(std::move(canvasScene));

    int cw = viewportWidth_, ch = viewportHeight_;
    {
        auto& box = canvas->layoutBox();
        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
    }

    auto graph = std::make_unique<scene::SceneGraph>();
    graph->setCanvasScene(csPtr);
    graph->setPhysicsWorld(physicsWorld_.get());
    graph->setCanvasSize(cw, ch);
    auto* graphPtr = graph.get();

    canvas->setSceneGraph(graphPtr);
    graphPtr->setFBOTextureCallback([canvas](unsigned int tex) {
        canvas->setSceneGraphFBOTexture(tex);
    });
    graphPtr->setGizmoProvider([this](scene::SceneGraph* g) {
        return gizmo_ ? gizmo_->meshesForRender(g)
                      : std::vector<scene::MeshNode*>{};
    });

    sceneGraphs_.push_back({std::move(graph), canvas,
                            canvas->document(), canvas->nodeId()});
    return graphPtr;
#endif  // BRO_WITH_3D
}

size_t Engine::sceneContextCount() const {
#if BRO_WITH_3D
    return sceneGraphs_.size();
#else
    return 0;
#endif
}

#if BRO_WITH_3D
dom::Element* Engine::liveElementOf(const SceneGraphEntry& entry) const {
    if (!entry.element || !entry.document) return nullptr;
    if (!dom::Document::isLiveDocument(entry.document)) return nullptr;
    auto* node = entry.document->resolveNode(entry.element, entry.elementId);
    return node ? static_cast<dom::Element*>(node) : nullptr;
}

static void severSceneGraphLink(dom::Element* el) {
    if (!el) return;
    el->setSceneGraph(nullptr);
    el->setSceneGraphFBOTexture(0);
}

void Engine::pruneDetachedSceneGraphs() {
    sceneGraphs_.erase(
        std::remove_if(sceneGraphs_.begin(), sceneGraphs_.end(),
            [this](SceneGraphEntry& sg) {
                if (!sg.element || !sg.document) return false;
                dom::Element* el = liveElementOf(sg);
                if (!el) return true;

                auto* n = el;
                while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                const bool detached =
                    n->tagName() != "html" && n->tagName() != "HTML";
                if (detached) severSceneGraphLink(el);
                return detached;
            }),
        sceneGraphs_.end());
}

void Engine::clearSceneGraphs() {
    for (auto& sg : sceneGraphs_) severSceneGraphLink(liveElementOf(sg));
    sceneGraphs_.clear();
}
#endif  // BRO_WITH_3D

dom::ListenerHandle Engine::addWindowEventListener(const std::string& type,
                                                   dom::EventCallback cb,
                                                   dom::ListenerOptions opts) {
    if (!document_) return dom::ListenerHandle{};
    return document_->windowListeners().add(type, std::move(cb), opts);
}

bool Engine::removeWindowEventListener(dom::ListenerHandle handle) {
    if (!document_) return false;
    return document_->windowListeners().remove(handle);
}

void Engine::resetMenuBarDefaults() {
    menuBar_.roots.clear();

    MenuBar::Item file;
    file.id = "file"; file.label = "File";
    MenuBar::Item quit;
    quit.id = "__system.quit"; quit.label = "Quit"; quit.accel = "Ctrl+Q";
    file.children.push_back(std::move(quit));

    MenuBar::Item edit;
    edit.id = "edit"; edit.label = "Edit";
    MenuBar::Item prefs;
    prefs.id = "__system.preferences"; prefs.label = "Preferences...";
    edit.children.push_back(std::move(prefs));

    MenuBar::Item view;
    view.id = "view"; view.label = "View";
    MenuBar::Item insp;
    insp.id = "__system.inspector"; insp.label = "Inspector";
    view.children.push_back(std::move(insp));

    menuBar_.roots.push_back(std::move(file));
    menuBar_.roots.push_back(std::move(edit));
    menuBar_.roots.push_back(std::move(view));
    menuBar_.dirty = true;
}

void Engine::loadCustomFonts() {
    if (!document_ || !renderer_) return;
    auto& fontFaces = document_->cascade().fontFaces();
    std::string basePath = document_->basePath();

    for (auto& ff : fontFaces) {
        bool alreadyLoaded = std::any_of(
            loadedFonts_.begin(), loadedFonts_.end(),
            [&](const LoadedFont& lf) {
                return lf.family == ff.family && lf.weight == ff.weight &&
                       lf.italic == ff.italic;
            });
        if (alreadyLoaded) continue;

        std::string path = AppLoader::resolvePath(basePath, ff.src, &assetMounts_);

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            LOG_WARN("Failed to load @font-face '%s' from '%s'", ff.family.c_str(), path.c_str());
            continue;
        }
        auto size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> data(size);
        file.read(data.data(), size);
        file.close();

        if (renderer_->registerCustomFont(ff.family, data.data(), data.size(),
                                          ff.weight, ff.italic)) {
            LOG_INFO("Loaded @font-face '%s' from '%s'", ff.family.c_str(), path.c_str());
            loadedFonts_.push_back({ff.family, data, ff.weight, ff.italic});
        }
    }
}
} // namespace bro::engine
