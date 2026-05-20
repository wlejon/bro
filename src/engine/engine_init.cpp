#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/inspector_highlight.h"
#include "engine/key_mapping.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "engine/default_styles.h"

#include <filesystem>
#include <fstream>

#include "platform/sdl_window.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "js/runtime.h"
#include "js/timers.h"
#include "js/dom_bindings.h"
#include "js/canvas_bindings.h"
#include "js/event_dispatch.h"
#include "js/audio_bindings.h"
#include "js/storage_bindings.h"
#include "js/settings_bindings.h"
#include "js/dialog_bindings.h"
#include "js/window_bindings.h"
#include "js/custom_elements.h"
#include "js/webgl2_bindings.h"
#include "js/image_bindings.h"
#include "js/video_bindings.h"
#include "js/worker.h"
#include "js/physics_bindings.h"
#include "js/scene_bindings.h"
#include "js/crosshair_bindings.h"
#include "js/menu_bindings.h"
#include "js/gizmo_bindings.h"
#include "js/mesh_bindings.h"
#include "js/flora_bindings.h"
#include "js/math_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/diffusion_bindings.h"
#include "js/terrain_bindings.h"
#include "js/net_bindings.h"
#include "js/server_bindings.h"
#include "js/headless_bindings.h"

#include "physics/physics_world.h"
#include "net/net_service.h"
#include "scene/scene_graph.h"
#include "api/api.h"
#include "runtime/runtime.h"
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

    // === Asset mounts (engine-supplied virtual paths: /lib, /system, ...) ===
    // Project-root mounts come first; app-local overrides applied after the
    // app dir is known to exist.
    {
        namespace fs = std::filesystem;
        auto tryMount = [&](const std::string& prefix, const std::string& dirName) {
            // App-local override has highest priority.
            if (!config.appDir.empty()) {
                fs::path appLocal = fs::path(config.appDir) / dirName;
                std::error_code ec;
                if (fs::is_directory(appLocal, ec)) {
                    assetMounts_.addMount("/" + prefix, fs::absolute(appLocal, ec).string());
                    return;
                }
            }
            // Project-root mount.
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
    }

    // === Settings system ===
    settings_ = std::make_unique<Settings>(config.settingsPath);

    // Define engine-level actions (lowest priority — apps can override)
    settings_->defineEngineAction("system_toggle_perf", {"F8"});
    // Settings overlay is reached via the File/Edit menu by default; ESC is
    // no longer reserved. Users may rebind this action from settings.
    settings_->defineEngineAction("system_toggle_settings", {});

    settings_->applyAppOverrides(config.graphics, config.input);

    // Use resolved settings for window creation
    auto& gfx = settings_->graphics();
    auto& inp = settings_->input();
    viewportWidth_ = gfx.width;
    viewportHeight_ = gfx.height;
    uiFrameIntervalMs_ = gfx.maxFrameIntervalMs;
    inputConfig_.scrollSpeed = inp.scrollSpeed;
    inputConfig_.doubleClickThresholdMs = inp.doubleClickThresholdMs;
    inputConfig_.doubleClickDistancePx = inp.doubleClickDistancePx;
    inputConfig_.overlayToggleKey = inp.overlayToggleKey;

    // === Common JS runtime initialization ===

    // 4. JS runtime + bindings
    jsRuntime_ = std::make_unique<js::Runtime>();
    jsRuntime_->setModuleLoader();

    // Wire brokit logging through bro's LOG_* macros
    brokit::Runtime::setLogCallback([](brokit::Runtime::LogLevel level, const std::string& msg) {
        switch (level) {
            case brokit::Runtime::LogLevel::Warn:  LOG_WARN("[console] %s", msg.c_str()); break;
            case brokit::Runtime::LogLevel::Error: LOG_ERROR("[console] %s", msg.c_str()); break;
            default: LOG_INFO("[console] %s", msg.c_str()); break;
        }
    });

    // Install all brokit APIs (console, timers, URL, crypto, encoding, fetch, etc.)
    brokit::api::installAll(jsRuntime_->getContext());

    timers_ = std::make_unique<js::Timers>();
    js::Timers::install(jsRuntime_->getContext(), timers_.get());

    // Seed the timer time base so setTimeout/setInterval use the correct clock.
    // In headless mode this is virtual time; in windowed/server mode, real time.
    timers_->tick(displayMode_ == DisplayMode::Headless ? virtualTime_ : util::currentTimeMs());

    // Physics engine + bindings (all modes)
    physicsWorld_ = std::make_unique<physics::PhysicsWorld>();
    physicsWorld_->init();
    if (displayMode_ != DisplayMode::Headless)
        physicsWorld_->startThread();
    js::PhysicsBindings::install(jsRuntime_->getContext(), physicsWorld_.get());

    // Mesh bindings (standalone Mesh class wrapping bromesh — all modes)
    js::MeshBindings::install(jsRuntime_->getContext());

    // Flora bindings (broflora ecosystem sim — bro.flora.* — all modes)
    js::FloraBindings::install(jsRuntime_->getContext());

    // Math bindings (bro.math.* — SpatialHash3D and future bromath types)
    js::MathBindings::install(jsRuntime_->getContext());

    // Rigging bindings (SkinData, VoxelChunk; later: Skeleton/Pose/Animation/IK/Rig)
    js::RiggingBindings::install(jsRuntime_->getContext());

    // AI bindings (game agent: navgrid, pathfinding, steering — all modes)
    js::AIBindings::install(jsRuntime_->getContext());

    // bro.tensor (GPU tensor + ops via brotensor sibling). Real bindings when
    // a backend is enabled at configure time; stub `{ available: false }` otherwise.
    js::installTensorBindings(jsRuntime_->getContext());

    // bro.diffusion (diffusion-model inference via brodiffusion sibling).
    // Always real — brodiffusion's CPU backend is always built. Main thread
    // drives the step-wise inspection API; workers install the same binding
    // for fast full generation.
    js::installDiffusionBindings(jsRuntime_->getContext());

    // Terrain bindings (infinite voxel terrain system — all modes)
    js::TerrainBindings::install(jsRuntime_->getContext());

    // Network service + bindings (all modes). NetService owns GNS on its own
    // thread; bindings hold a per-context subscriber that polls each frame.
    netService_ = std::make_unique<net::NetService>();
    js::NetBindings::install(jsRuntime_->getContext(), netService_.get());

    // bro.server.* (all modes) — in windowed mode this lets the process host
    // an in-process server script (e.g. the launcher running apps/fps/server.js).
    serverStartTime_ = util::currentTimeMs();
    js::ServerBindings::install(jsRuntime_->getContext(), this);

    // === Server mode: lightweight init — no rendering, DOM, or audio ===

    if (displayMode_ == DisplayMode::Server) {
        // Storage (persisted key/value)
        std::string storagePath = config.appDir + "/.storage.json";
        js::StorageBindings::install(jsRuntime_->getContext(), storagePath);
        js::StorageBindings::installSessionStorage(jsRuntime_->getContext());

        // Settings JS API
        js::SettingsBindings::install(jsRuntime_->getContext(), settings_.get(), nullptr);

        // Register app directory as base path for fetch and fs
        brokit::api::addFetchBasePath(jsRuntime_->getContext(), config.appDir);
        brokit::api::addFsBasePath(jsRuntime_->getContext(), config.appDir);
        for (const auto& [prefix, target] : assetMounts_.mounts()) {
            brokit::api::addFsPrefixMount(jsRuntime_->getContext(), prefix, target);
            brokit::api::addFetchPrefixMount(jsRuntime_->getContext(), prefix, target);
        }

        // Worker bindings
        js::installWorkerBindings(jsRuntime_->getContext(), config.appDir,
                                  netService_.get(), &assetMounts_);

        LOG_INFO("Server mode initialized (no rendering, no DOM, no audio)");
        return;
    }

    // === Windowed / Headless initialization (rendering + DOM) ===

    // hasGL: true when we have a GPU context (windowed, or headless with GPU)
    const bool hasGL = (displayMode_ == DisplayMode::Windowed) || config.graphics.useGPU;

    if (hasGL) {
        // Create window (hidden for headless, visible for windowed)
        bool hidden = (displayMode_ == DisplayMode::Headless);
        window_ = std::make_unique<platform::Window>("Bro",
            static_cast<uint32_t>(gfx.width),
            static_cast<uint32_t>(gfx.height), hidden,
            gfx.resizable, gfx.vsync);

        // Taskbar / Alt-Tab icon. Shipped with system/ alongside the binary
        // (scripts/package-release.sh copies the whole system/ tree). Skip in
        // headless where the window is hidden anyway.
        if (!hidden) {
            window_->setIcon("system/icon.png");
        }

        // GL context (shader programs + helpers)
        gl_ = std::make_unique<render::GLContext>(*window_);

        // Renderer (Skia raster + OpenGL display)
        renderer_ = render::createRenderer(gl_.get());
        if (!renderer_) {
            throw std::runtime_error("Failed to create renderer");
        }
    } else {
        // No GPU: CPU-only Skia renderer, no window/GL
        renderer_ = std::make_unique<render::RasterRenderer>();
    }

    if (displayMode_ == DisplayMode::Headless) {
        virtualTime_ = util::currentTimeMs();
    }

    // 4b. Audio engine + bindings
    audioEngine_ = std::make_unique<broaudio::Engine>();
    if (displayMode_ == DisplayMode::Windowed) {
        audioEngine_->init();
    } else {
        audioEngine_->initHeadless();
    }
    js::AudioBindings::install(jsRuntime_->getContext(), audioEngine_.get());

    // Apply initial audio settings from persisted user overrides
    {
        auto& audio = settings_->audio();
        float vol = audio.muted ? 0.0f : audio.masterVolume;
        audioEngine_->setMasterGain(vol);
    }

    // Register settings change callback for runtime changes
    settings_->setChangeCallback([this](const std::string& category,
                                        const std::string& key) {
        if (category == "graphics" || category == "*") {
            auto& gfx = settings_->graphics();
            if ((key == "fullscreen" || key == "*") && window_) {
                window_->setFullscreen(gfx.fullscreen);
                setFullscreenState(gfx.fullscreen);
            }
            if ((key == "vsync" || key == "*") && window_)
                window_->setVSync(gfx.vsync);
            if ((key == "width" || key == "height" || key == "*") && window_ && !gfx.fullscreen)
                window_->setWindowSize(static_cast<uint32_t>(gfx.width),
                                       static_cast<uint32_t>(gfx.height));
            if ((key == "resizable" || key == "*") && window_)
                window_->setResizable(gfx.resizable);
            if (key == "maxFrameIntervalMs" || key == "*")
                uiFrameIntervalMs_ = gfx.maxFrameIntervalMs;
        }
        if (category == "audio" || category == "*") {
            auto& audio = settings_->audio();
            float vol = audio.muted ? 0.0f : audio.masterVolume;
            audioEngine_->setMasterGain(vol);
        }
        if (category == "input" || category == "*") {
            auto& inp = settings_->input();
            inputConfig_.scrollSpeed = inp.scrollSpeed;
            inputConfig_.doubleClickThresholdMs = inp.doubleClickThresholdMs;
            inputConfig_.doubleClickDistancePx = inp.doubleClickDistancePx;
            inputConfig_.overlayToggleKey = inp.overlayToggleKey;
        }
    });

    // Scene graph bindings (windowed/headless only — needs renderer)
    js::SceneBindings::install(jsRuntime_->getContext());

    // Crosshair bindings (bro.crosshair.*)
    js::CrosshairBindings::install(jsRuntime_->getContext(), this);

    // Menu bar bindings (bro.menu.*) + default menu tree.
    js::MenuBindings::install(jsRuntime_->getContext(), this);
    {
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

    // Engine gizmo (bro.gizmo.*) — translate arrows for now; rotate + scale
    // handles + mouse-driven interaction land in later phases.
    gizmo_ = std::make_unique<GizmoManager>();
    js::GizmoBindings::install(jsRuntime_->getContext(), this);

    // 5. Layout helpers. drawTraversal_ writes through a RecordingRenderer
    //    (wraps renderer_ for measurement + font handle issuance) so the paint
    //    walk emits commands into a CommandBuffer instead of issuing Skia
    //    work — the raster thread replays the buffer without touching the DOM.
    //    textMetrics_ stays bound to renderer_ since it only does measurement.
    recordingRenderer_ = std::make_unique<render::RecordingRenderer>(nullptr, renderer_.get());
    drawTraversal_ = std::make_unique<layout::DrawTraversal>(recordingRenderer_.get());
    textMetrics_ = std::make_unique<layout::SkiaTextMetrics>(renderer_.get());

    // 6. Load the application
    manifest_ = AppLoader::loadApp(config.appDir, &assetMounts_);
    std::string html = AppLoader::loadFile(manifest_.htmlPath);
    if (html.empty()) {
        throw std::runtime_error("Failed to load index.html from " + config.appDir);
    }

    // 4c. localStorage (persisted) + sessionStorage (in-memory)
    std::string storagePath = manifest_.basePath + "/.storage.json";
    js::StorageBindings::install(jsRuntime_->getContext(), storagePath);
    js::StorageBindings::installSessionStorage(jsRuntime_->getContext());

    // Settings JS API (bro.settings.*)
    js::SettingsBindings::install(jsRuntime_->getContext(), settings_.get(),
                                  window_ ? window_.get() : nullptr);

    // Set the base path so relative paths work.
    drawTraversal_->setBasePath(manifest_.basePath);
    drawTraversal_->setViewport(contentWidth(), contentHeight(), contentTop());

    // Load user stylesheets separately from UA defaults.
    // UA defaults use UserAgent origin (lowest priority) so any author
    // rule — even `* { margin: 0 }` — overrides them correctly.
    std::string authorStyles;
    for (auto& cssPath : manifest_.stylePaths) {
        std::string css = AppLoader::loadFile(cssPath);
        if (!css.empty()) {
            authorStyles += css + "\n";
        }
    }

    // 7. Extract <template> blocks before parsing (gumbo discards them)
    std::vector<dom::Document::TemplateBlock> templateBlocks;
    html = dom::Document::extractTemplates(html, templateBlocks);

    // 8. Parse HTML and build bro::dom tree
    document_ = std::make_unique<dom::Document>();
    document_->setBasePath(manifest_.basePath);
    document_->parse(html, authorStyles, kDefaultStyles);

    // 8a. Inject extracted templates back into the DOM tree
    if (!templateBlocks.empty())
        document_->injectTemplates(templateBlocks);

    // 8b. Set window title (windowed only)
    if (window_) {
        if (!config.title.empty()) {
            window_->setTitle(config.title);
        } else {
            std::string docTitle = document_->title();
            if (!docTitle.empty()) {
                window_->setTitle(docTitle);
            }
        }
    }

    // 9. Set up window/navigator/location/history BEFORE DOM bindings
    js::installWindowBindings(jsRuntime_->getContext(), viewportWidth_, contentHeight());

    // 9x. Native file dialogs (modal to our SDL window).
    //      Pass a tick callback so JS timers keep running while dialog is open.
    js::DialogBindings::install(jsRuntime_->getContext(),
                                window_ ? window_->getSDLWindow() : nullptr,
                                [this]() { tickTimersOnly(); });

    // 9a. Install DOM JS bindings (after window so polyfills work)
    js::DomBindings::install(jsRuntime_->getContext(), document_.get());
    js::DomBindings::setSDLWindow(jsRuntime_->getContext(),
                                  window_ ? window_->getSDLWindow() : nullptr);
    js::DomBindings::setEngine(jsRuntime_->getContext(), this);

    // 9b. Install custom elements (after DOM bindings — needs element class ID)
    js::installCustomElements(jsRuntime_->getContext(),
                              js::DomBindings::elementClassId(), document_.get());

    // 9c. Install Canvas 2D bindings + getContext factory
    js::CanvasBindings::install(jsRuntime_->getContext());
    js::ImageBindings::install(jsRuntime_->getContext(), manifest_.basePath, &assetMounts_);
    js::VideoBindings::install(jsRuntime_->getContext(), manifest_.basePath);
    js::SceneBindings::setAppContext(manifest_.basePath, &assetMounts_);
    // screenshotCanvas works on both GPU-backed (windowed) and raster
    // (headless) Skia surfaces, so it's installed in both modes — apps can
    // use it for in-app capture / save flows without a headless drive.
    js::installCanvasSnapshotBinding(jsRuntime_->getContext(), this);

    // Register app directory as base path for fetch and fs (overlay: last added = checked first)
    brokit::api::addFetchBasePath(jsRuntime_->getContext(), manifest_.basePath);
    brokit::api::addFsBasePath(jsRuntime_->getContext(), manifest_.basePath);

    // Register engine-supplied prefix mounts (/lib, /system, ...) so fs and
    // fetch resolve them ahead of basePath.
    for (const auto& [prefix, target] : assetMounts_.mounts()) {
        brokit::api::addFsPrefixMount(jsRuntime_->getContext(), prefix, target);
        brokit::api::addFetchPrefixMount(jsRuntime_->getContext(), prefix, target);
    }

    if (gl_) {
        // GPU path: WebGL2 + full canvas factory (windowed or GPU headless)
        js::WebGL2Bindings::install(jsRuntime_->getContext());
        js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
            [this](JSContext* ctx, dom::Element* el, const std::string& type) -> JSValue {
                if (type == "2d") {
                    auto scene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                    if (el) {
                        scene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                            auto* elem = static_cast<dom::Element*>(ud);
                            if (!elem->parentNode()) {
                                ox = oy = ow = oh = 0;
                                return;
                            }
                            auto& box = elem->layoutBox();
                            ox = box.contentRect.x;
                            oy = box.contentRect.y;
                            for (auto* lp = elem->layoutParent(); lp; lp = lp->layoutParent()) {
                                auto& pb = lp->layoutBox();
                                ox += pb.contentRect.x;
                                oy += pb.contentRect.y;
                                oy -= lp->scrollTopValue();
                            }
                            ow = box.contentRect.width;
                            oh = box.contentRect.height;
                        }, el);
                        scene->setDetachedCallback([](void* ud) -> bool {
                            // Walk up to check if connected to the document
                            auto* n = static_cast<dom::Element*>(ud);
                            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                            return n->tagName() != "html" && n->tagName() != "HTML";
                        }, el);
                    }
                    auto* ptr = scene.get();
                    if (el) el->setCanvasScene(ptr);
                    addCanvasScene(std::move(scene));
                    return js::CanvasBindings::wrapContext2D(ctx, ptr);
                }
                if (type == "webgl2" || type == "webgl") {
                    // Size to element layout (fall back to viewport if no layout yet)
                    int cw = viewportWidth_, ch = viewportHeight_;
                    if (el) {
                        auto& box = el->layoutBox();
                        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
                        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
                    }
                    auto ctx2 = std::make_unique<webgl::WebGL2RenderingContext>(cw, ch);
                    auto* webglCtx = ctx2.get();
                    if (el) el->setWebglContext(webglCtx);
                    webglEntries_.push_back({std::move(ctx2), el});
                    return js::WebGL2Bindings::wrapContext(ctx, webglCtx);
                }
                if (type == "scene") {
                    // Create a 2D canvas for the scene graph to render into
                    auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                    if (el) {
                        canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                            auto* elem = static_cast<dom::Element*>(ud);
                            if (!elem->parentNode()) {
                                ox = oy = ow = oh = 0;
                                return;
                            }
                            auto& box = elem->layoutBox();
                            ox = box.contentRect.x;
                            oy = box.contentRect.y;
                            for (auto* lp = elem->layoutParent(); lp; lp = lp->layoutParent()) {
                                auto& pb = lp->layoutBox();
                                ox += pb.contentRect.x;
                                oy += pb.contentRect.y;
                                oy -= lp->scrollTopValue();
                            }
                            ow = box.contentRect.width;
                            oh = box.contentRect.height;
                        }, el);
                        canvasScene->setDetachedCallback([](void* ud) -> bool {
                            auto* n = static_cast<dom::Element*>(ud);
                            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                            return n->tagName() != "html" && n->tagName() != "HTML";
                        }, el);
                    }
                    auto* csPtr = canvasScene.get();
                    if (el) el->setCanvasScene(csPtr);
                    addCanvasScene(std::move(canvasScene));

                    // Size to element layout (fall back to viewport)
                    int cw = viewportWidth_, ch = viewportHeight_;
                    if (el) {
                        auto& box = el->layoutBox();
                        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
                        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
                    }

                    auto graph = std::make_unique<scene::SceneGraph>();
                    graph->setCanvasScene(csPtr);
                    graph->setPhysicsWorld(physicsWorld_.get());
                    graph->setCanvasSize(cw, ch);
                    auto* graphPtr = graph.get();
                    if (el) {
                        el->setSceneGraph(graphPtr);
                        graphPtr->setFBOTextureCallback([el](unsigned int tex) {
                            el->setSceneGraphFBOTexture(tex);
                        });
                    }
                    graphPtr->setGizmoProvider([this](scene::SceneGraph* g) {
                        return gizmo_ ? gizmo_->meshesForRender(g)
                                       : std::vector<scene::MeshNode*>{};
                    });
                    sceneGraphs_.push_back({std::move(graph), el});
                    return js::SceneBindings::wrapSceneGraph(ctx, graphPtr);
                }
                return JS_NULL;
            });
    } else {
        // CPU path: 2D canvas + scene graph, no WebGL
        js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
            [this](JSContext* ctx, dom::Element* el, const std::string& type) -> JSValue {
                auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                if (el) {
                    canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                        auto* elem = static_cast<dom::Element*>(ud);
                        if (!elem->parentNode()) {
                            ox = oy = ow = oh = 0;
                            return;
                        }
                        auto& box = elem->layoutBox();
                        ox = box.contentRect.x;
                        oy = box.contentRect.y;
                        for (auto* lp = elem->layoutParent(); lp; lp = lp->layoutParent()) {
                            auto& pb = lp->layoutBox();
                            ox += pb.contentRect.x;
                            oy += pb.contentRect.y;
                            oy -= lp->scrollTopValue();
                        }
                        ow = box.contentRect.width;
                        oh = box.contentRect.height;
                    }, el);
                    canvasScene->setDetachedCallback([](void* ud) -> bool {
                        auto* n = static_cast<dom::Element*>(ud);
                        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                        return n->tagName() != "html" && n->tagName() != "HTML";
                    }, el);
                }
                auto* csPtr = canvasScene.get();
                if (el) el->setCanvasScene(csPtr);
                canvasScene->init(nullptr);
                canvasScenes_.push_back(std::move(canvasScene));
                if (type == "scene") {
                    int cw = viewportWidth_, ch = viewportHeight_;
                    if (el) {
                        auto& box = el->layoutBox();
                        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
                        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
                    }
                    auto graph = std::make_unique<scene::SceneGraph>();
                    graph->setCanvasScene(csPtr);
                    graph->setPhysicsWorld(physicsWorld_.get());
                    graph->setCanvasSize(cw, ch);
                    auto* graphPtr = graph.get();
                    if (el) {
                        el->setSceneGraph(graphPtr);
                        graphPtr->setFBOTextureCallback([el](unsigned int tex) {
                            el->setSceneGraphFBOTexture(tex);
                        });
                    }
                    graphPtr->setGizmoProvider([this](scene::SceneGraph* g) {
                        return gizmo_ ? gizmo_->meshesForRender(g)
                                       : std::vector<scene::MeshNode*>{};
                    });
                    sceneGraphs_.push_back({std::move(graph), el});
                    return js::SceneBindings::wrapSceneGraph(ctx, graphPtr);
                }
                return js::CanvasBindings::wrapContext2D(ctx, csPtr);
            });
    }

    // 9d. Install Worker bindings
    js::installWorkerBindings(jsRuntime_->getContext(), manifest_.basePath,
                              netService_.get(), &assetMounts_);

    // 10. Load and execute scripts (external + inline, in document order)
    for (auto& script : manifest_.scripts) {
        if (script.isInline()) {
            if (!jsRuntime_->eval(script.code, "<inline>")) {
                LOG_ERROR("Failed to execute inline script");
            }
        } else {
            std::string code = AppLoader::loadFile(script.path);
            if (!code.empty()) {
                if (!jsRuntime_->eval(code, script.path)) {
                    LOG_ERROR("Failed to execute script: %s", script.path.c_str());
                }
            }
        }
    }

    // 10a. Dispatch DOMContentLoaded on document
    {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);
        // Fire DOMContentLoaded on documentElement so document.addEventListener
        // callers (the standard web idiom) receive it via bubbling.
        if (auto* root = document_ ? document_->documentElement() : nullptr) {
            bro::dom::Event dclDom("DOMContentLoaded", /*bubbles=*/true, /*cancelable=*/false);
            js::dispatchDomEvent(ctx, root, dclDom);
        }

        JSValue dispatch = JS_GetPropertyStr(ctx, global, "__bro_dispatch_window_event");
        if (JS_IsFunction(ctx, dispatch)) {
            // DOMContentLoaded (window-level listeners)
            JSValue dclType = JS_NewString(ctx, "DOMContentLoaded");
            JSValue dclEvt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, dclEvt, "type", JS_NewString(ctx, "DOMContentLoaded"));
            JS_SetPropertyStr(ctx, dclEvt, "bubbles", JS_TRUE);
            JS_SetPropertyStr(ctx, dclEvt, "cancelable", JS_FALSE);
            JS_SetPropertyStr(ctx, dclEvt, "isTrusted", JS_TRUE);
            JS_SetPropertyStr(ctx, dclEvt, "target", JS_DupValue(ctx, global));
            JSValue dclArgs[2] = { dclType, dclEvt };
            JSValue dclRet = JS_Call(ctx, dispatch, global, 2, dclArgs);
            JS_FreeValue(ctx, dclRet);
            JS_FreeValue(ctx, dclType);
            JS_FreeValue(ctx, dclEvt);

            // load
            JSValue loadType = JS_NewString(ctx, "load");
            JSValue loadEvt = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, loadEvt, "type", JS_NewString(ctx, "load"));
            JS_SetPropertyStr(ctx, loadEvt, "bubbles", JS_FALSE);
            JS_SetPropertyStr(ctx, loadEvt, "cancelable", JS_FALSE);
            JS_SetPropertyStr(ctx, loadEvt, "isTrusted", JS_TRUE);
            JS_SetPropertyStr(ctx, loadEvt, "target", JS_DupValue(ctx, global));
            JSValue loadArgs[2] = { loadType, loadEvt };
            JSValue loadRet = JS_Call(ctx, dispatch, global, 2, loadArgs);
            JS_FreeValue(ctx, loadRet);
            JS_FreeValue(ctx, loadType);
            JS_FreeValue(ctx, loadEvt);
        }
        JS_FreeValue(ctx, dispatch);
        JS_FreeValue(ctx, global);
        jsRuntime_->executePendingJobs();
    }

    // === Mode-specific post-init ===

    if (displayMode_ == DisplayMode::Windowed) {
        // 11. Event loop
        eventLoop_ = std::make_unique<platform::EventLoop>();

        // Install window.close() now that event loop exists
        js::installWindowClose(jsRuntime_->getContext(), eventLoop_.get());
    }

    // UI overlay quad VAO/VBO — used by compositeLayers (windowed main loop
    // and headless screenshot path both go through it).
    if (gl_) {
        glGenVertexArrays(1, &uiQuadVAO_);
        glGenBuffers(1, &uiQuadVBO_);
    }

    // 12. System panels (loads from system/ sibling directory)
    //     Shares the JS runtime — each panel gets its own JSContext.
    initSystemPanels();

    // 12b. Enable the startup splash. It renders above the app canvas and
    //      menu bar until its own JS finishes the swirl-away animation (or a
    //      hard timeout in tickSystemPanels fires). Enabled in both windowed
    //      and headless modes — headless uses virtual time, so `advanceTime()`
    //      drives the splash forward just like any other timer.
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

    // Load @font-face custom fonts from the cascade
    loadCustomFonts();

    // Headless: do initial layout + flush
    if (displayMode_ == DisplayMode::Headless) {
        ensureReplacedElements(document_->documentElement());
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_);
        document_->setTransitionManager(&transitionManager_, virtualTime_);
        animationManager_.setKeyframes(&document_->cascade().keyframes());
        document_->setAnimationManager(&animationManager_);
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(viewportWidth_),
                                 static_cast<float>(contentHeight()), *textMetrics_);
        flush();
    }
    // User script has not run yet during construction. Arm media events so
    // the next pump (first JS-driven flush / first main-loop tick) fires
    // queued loadedmetadata / timeupdate.
    mediaEventsArmed_ = true;
}

void Engine::loadCustomFonts() {
    if (!document_ || !renderer_) return;
    auto& fontFaces = document_->cascade().fontFaces();
    std::string basePath = document_->basePath();

    for (auto& ff : fontFaces) {
        std::string path = AppLoader::resolvePath(basePath, ff.src, &assetMounts_);

        // Read font file
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
