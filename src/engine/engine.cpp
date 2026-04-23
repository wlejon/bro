#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"

#include <fstream>

#include "observer_check.js.h"
#include "canvas_resize.js.h"

#include "platform/sdl_window.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
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
#include "js/worker.h"
#include "js/physics_bindings.h"
#include "js/scene_bindings.h"
#include "js/crosshair_bindings.h"
#include "js/menu_bindings.h"
#include "js/gizmo_bindings.h"
#include "js/mesh_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/terrain_bindings.h"
#include "js/net_bindings.h"
#include "js/server_bindings.h"

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
#include "dom/node.h"
#include "dom/event.h"
#include "dom/shadow_root.h"
#include "dom/range.h"
#include "dom/selection.h"
#include "layout/selection_geometry.h"

#include <cstring>
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "layout/element_ref_adapter.h"
#include "layout/layout_node_adapter.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "engine/default_styles.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <stb_image_write.h>

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSurface.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <glad/gl.h>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

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

    // Rigging bindings (SkinData, VoxelChunk; later: Skeleton/Pose/Animation/IK/Rig)
    js::RiggingBindings::install(jsRuntime_->getContext());

    // AI bindings (game agent: navgrid, pathfinding, steering — all modes)
    js::AIBindings::install(jsRuntime_->getContext());

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

        // Worker bindings
        js::installWorkerBindings(jsRuntime_->getContext(), config.appDir,
                                  netService_.get());

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

        menuBar_.roots.push_back(std::move(file));
        menuBar_.roots.push_back(std::move(edit));
        menuBar_.dirty = true;
    }

    // Engine gizmo (bro.gizmo.*) — translate arrows for now; rotate + scale
    // handles + mouse-driven interaction land in later phases.
    gizmo_ = std::make_unique<GizmoManager>();
    js::GizmoBindings::install(jsRuntime_->getContext(), this);

    // 5. Layout helpers
    drawTraversal_ = std::make_unique<layout::DrawTraversal>(renderer_.get(), &fontManager_);
    textMetrics_ = std::make_unique<layout::SkiaTextMetrics>(renderer_.get(), &fontManager_);

    // 6. Load the application
    manifest_ = AppLoader::loadApp(config.appDir);
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
    drawTraversal_->setViewport(viewportWidth_, contentHeight(), contentTop());

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
    js::ImageBindings::install(jsRuntime_->getContext(), manifest_.basePath);

    // Register app directory as base path for fetch and fs (overlay: last added = checked first)
    brokit::api::addFetchBasePath(jsRuntime_->getContext(), manifest_.basePath);
    brokit::api::addFsBasePath(jsRuntime_->getContext(), manifest_.basePath);

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
                              netService_.get());

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

        // 13. Create UI overlay quad VAO/VBO
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
        // Resolve relative URL against app base path
        std::string path = ff.src;
        if (!path.empty() && path[0] != '/' && path.find(':') == std::string::npos) {
            path = basePath + "/" + path;
        }

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

void Engine::addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene) {
    if (scene) {
        scene->init(gl_.get());
        // In windowed GPU mode, each canvas gets its own thread with a shared
        // GL context + GrDirectContext for parallel rasterization.
        if (displayMode_ == DisplayMode::Windowed && window_) {
            // Context creation on the main thread (macOS/AppKit requirement);
            // startThread() blocks until the worker has MakeCurrent'd it, so
            // the next createSharedContext call cannot overlap with a worker's
            // wgl*Context call (Windows/NVIDIA requirement).
            auto ctx = window_->createSharedContext();
            if (ctx) {
                scene->startThread(ctx, window_->getSDLWindow());
            }
        } else {
            // Headless / CPU fallback: use renderer's GrContext directly
            auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
            if (skia && skia->grContext()) {
                scene->setGrContext(skia->grContext());
            }
        }
    }
    canvasScenes_.push_back(std::move(scene));
}

void Engine::compositeCanvasScenes(int w, int h) {
    compositeCanvasScenes(gl_.get(), w, h, 0);
}

void Engine::compositeCanvasScenes(render::GLContext* gl, int w, int h, GLuint targetFBO) {
    if (!gl || canvasScenes_.empty()) return;

    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, w, h);

    glUseProgram(gl->textureProgram());
    float viewport[2] = {(float)w, (float)h};
    glUniform2fv(gl->textureViewportLoc(), 1, viewport);
    glUniform1i(gl->textureSamplerLoc(), 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (auto& cs : canvasScenes_) {
        GLuint tex = cs->texture();
        if (!tex) continue;

        float cx, cy, cw, ch;
        cs->getScreenRect(cx, cy, cw, ch);
        // Raster surface is top-down: V=0 at top, V=1 at bottom.
        render::TextureVertex quad[6] = {
            {cx,      cy,      0, 0}, {cx+cw, cy,      1, 0}, {cx+cw, cy+ch, 1, 1},
            {cx,      cy,      0, 0}, {cx+cw, cy+ch, 1, 1}, {cx,      cy+ch, 0, 1},
        };

        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);

        GLuint vao = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                              (void*)offsetof(render::TextureVertex, u));

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Engine::drawTexturedQuad(GLuint tex, float x, float y, float w, float h) {
    if (!tex || !gl_) return;

    render::TextureVertex quad[6] = {
        {x,   y,   0, 0}, {x+w, y,   1, 0}, {x+w, y+h, 1, 1},
        {x,   y,   0, 0}, {x+w, y+h, 1, 1}, {x,   y+h, 0, 1},
    };

    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);

    glBindVertexArray(uiQuadVAO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Engine::compositeLayers(const std::vector<UILayer>& layers) {
    if (!gl_) return;
    if (layers.empty()) return;

    float vw = static_cast<float>(viewportWidth_);
    float vh = static_cast<float>(viewportHeight_);

    glUseProgram(gl_->textureProgram());
    float viewport[2] = {vw, vh};
    glUniform2fv(gl_->textureViewportLoc(), 1, viewport);
    glUniform1i(gl_->textureSamplerLoc(), 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Bind VAO and set up vertex attribs once for all quads
    glBindVertexArray(uiQuadVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));
    glActiveTexture(GL_TEXTURE0);

    for (auto& layer : layers) {
        if (layer.type == UILayer::HTML) {
            if (layer.texture) {
                render::TextureVertex quad[6] = {
                    {0,  0,  0, 0}, {vw, 0,  1, 0}, {vw, vh, 1, 1},
                    {0,  0,  0, 0}, {vw, vh, 1, 1}, {0,  vh, 0, 1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, layer.texture);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        } else {
            // Canvas/WebGL layer — get texture from canvas scene or direct texture
            GLuint tex = 0;
            if (layer.canvasScene) {
                tex = layer.canvasScene->texture();
            } else {
                tex = layer.texture;  // WebGL direct texture
            }
            if (tex) {
                float cx = layer.cx, cy = layer.cy;
                float cw = layer.cw, ch = layer.ch;

                // WebGL textures are bottom-up (origin at lower-left) so flip V coords
                float v0 = layer.canvasScene ? 0.0f : 1.0f;
                float v1 = layer.canvasScene ? 1.0f : 0.0f;

                render::TextureVertex quad[6] = {
                    {cx,    cy,    0, v0}, {cx+cw, cy,    1, v0}, {cx+cw, cy+ch, 1, v1},
                    {cx,    cy,    0, v0}, {cx+cw, cy+ch, 1, v1}, {cx,    cy+ch, 0, v1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, tex);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Crosshair spread system
// ---------------------------------------------------------------------------

void CrosshairConfig::tick(float dtSec) {
    if (dtSec <= 0.0f) return;

    // Compute target spread
    float target;
    if (manualSpread >= 0.0f) {
        // Manual override — use directly, skip interpolation
        currentSpread = manualSpread;
        // Still decay bloom so it's ready if we switch back to auto
        currentBloom = std::max(0.0f, currentBloom - bloomDecay * dtSec);
        return;
    }

    target = (aiming && adsSpread >= 0.0f) ? adsSpread : spread;
    if (moving) target += moveSpread;
    target += currentBloom;

    // Decay bloom
    currentBloom = std::max(0.0f, currentBloom - bloomDecay * dtSec);

    // Exponential lerp toward target
    float alpha = 1.0f - expf(-lerpSpeed * dtSec);
    currentSpread += (target - currentSpread) * alpha;
}

// ---------------------------------------------------------------------------
// Crosshair rendering
// ---------------------------------------------------------------------------

void Engine::drawCrosshairGL() {
    if (!crosshair_.visible || !gl_) return;

    float cx = viewportWidth_ * 0.5f;
    float cy = viewportHeight_ * 0.5f;
    float vw = static_cast<float>(viewportWidth_);
    float vh = static_cast<float>(viewportHeight_);

    // Build colored rectangles as ColorVertex triangles.
    // Each rect = 6 vertices (2 triangles).
    std::vector<render::ColorVertex> verts;
    verts.reserve(96); // outline + fill, up to ~16 rects

    auto pushRect = [&](float x, float y, float w, float h,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f, fa = a / 255.0f;
        // Pre-multiply alpha for GL blending
        fr *= fa; fg *= fa; fb *= fa;
        float x2 = x + w, y2 = y + h;
        verts.push_back({x,  y,  fr, fg, fb, fa});
        verts.push_back({x2, y,  fr, fg, fb, fa});
        verts.push_back({x2, y2, fr, fg, fb, fa});
        verts.push_back({x,  y,  fr, fg, fb, fa});
        verts.push_back({x2, y2, fr, fg, fb, fa});
        verts.push_back({x,  y2, fr, fg, fb, fa});
    };

    auto pushCircle = [&](float ccx, float ccy, float radius, int segs,
                          uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f, fa = a / 255.0f;
        fr *= fa; fg *= fa; fb *= fa;
        for (int i = 0; i < segs; i++) {
            float a0 = (float)i / segs * 6.2831853f;
            float a1 = (float)(i + 1) / segs * 6.2831853f;
            verts.push_back({ccx, ccy, fr, fg, fb, fa});
            verts.push_back({ccx + radius * cosf(a0), ccy + radius * sinf(a0), fr, fg, fb, fa});
            verts.push_back({ccx + radius * cosf(a1), ccy + radius * sinf(a1), fr, fg, fb, fa});
        }
    };

    auto pushRing = [&](float ccx, float ccy, float innerR, float outerR, int segs,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float fr = r / 255.0f, fg = g / 255.0f, fb = b / 255.0f, fa = a / 255.0f;
        fr *= fa; fg *= fa; fb *= fa;
        for (int i = 0; i < segs; i++) {
            float a0 = (float)i / segs * 6.2831853f;
            float a1 = (float)(i + 1) / segs * 6.2831853f;
            float c0 = cosf(a0), s0 = sinf(a0);
            float c1 = cosf(a1), s1 = sinf(a1);
            // Two triangles per segment
            verts.push_back({ccx + innerR * c0, ccy + innerR * s0, fr, fg, fb, fa});
            verts.push_back({ccx + outerR * c0, ccy + outerR * s0, fr, fg, fb, fa});
            verts.push_back({ccx + outerR * c1, ccy + outerR * s1, fr, fg, fb, fa});
            verts.push_back({ccx + innerR * c0, ccy + innerR * s0, fr, fg, fb, fa});
            verts.push_back({ccx + outerR * c1, ccy + outerR * s1, fr, fg, fb, fa});
            verts.push_back({ccx + innerR * c1, ccy + innerR * s1, fr, fg, fb, fa});
        }
    };

    auto& ch = crosshair_;
    float ht = ch.thickness * 0.5f;
    float ot = ch.outline ? ch.outlineThickness : 0.0f;

    // Helper: emit cross arms (4 rectangles) with given color + optional outline expansion
    auto emitCrossArms = [&](float expand, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        float e = expand;
        // Right arm
        pushRect(cx + ch.currentSpread - e, cy - ht - e,
                 ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e, r, g, b, a);
        // Left arm
        pushRect(cx - ch.size - e, cy - ht - e,
                 ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e, r, g, b, a);
        // Bottom arm
        pushRect(cx - ht - e, cy + ch.currentSpread - e,
                 ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e, r, g, b, a);
        // Top arm
        pushRect(cx - ht - e, cy - ch.size - e,
                 ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e, r, g, b, a);
    };

    bool hasCross = (ch.style == CrosshairConfig::Cross || ch.style == CrosshairConfig::CrossDot);
    bool hasDot = (ch.style == CrosshairConfig::Dot || ch.style == CrosshairConfig::CrossDot);
    bool hasCircle = (ch.style == CrosshairConfig::Circle);
    int circleSegs = 32;

    // Outline pass
    if (ch.outline) {
        if (hasCross) {
            emitCrossArms(ot, ch.outR, ch.outG, ch.outB, ch.outA);
        }
        if (hasDot) {
            pushCircle(cx, cy, ch.dotSize + ot, circleSegs,
                       ch.outR, ch.outG, ch.outB, ch.outA);
        }
        if (hasCircle) {
            pushRing(cx, cy, ch.size - ch.thickness * 0.5f - ot,
                     ch.size + ch.thickness * 0.5f + ot, circleSegs,
                     ch.outR, ch.outG, ch.outB, ch.outA);
        }
    }

    // Fill pass
    if (hasCross) {
        emitCrossArms(0, ch.r, ch.g, ch.b, ch.a);
    }
    if (hasDot) {
        pushCircle(cx, cy, ch.dotSize, circleSegs, ch.r, ch.g, ch.b, ch.a);
    }
    if (hasCircle) {
        pushRing(cx, cy, ch.size - ch.thickness * 0.5f,
                 ch.size + ch.thickness * 0.5f, circleSegs,
                 ch.r, ch.g, ch.b, ch.a);
    }

    if (verts.empty()) return;

    // Draw using color pipeline
    glUseProgram(gl_->colorProgram());
    float viewport[2] = {vw, vh};
    glUniform2fv(gl_->colorViewportLoc(), 1, viewport);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(uiQuadVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(render::ColorVertex)),
                 verts.data(), GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::ColorVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                          sizeof(render::ColorVertex),
                          (void*)offsetof(render::ColorVertex, r));

    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
}

void Engine::drawCrosshairSkia(SkCanvas* canvas) {
    if (!crosshair_.visible || !canvas) return;

    float cx = viewportWidth_ * 0.5f;
    float cy = viewportHeight_ * 0.5f;
    auto& ch = crosshair_;
    float ht = ch.thickness * 0.5f;
    float ot = ch.outline ? ch.outlineThickness : 0.0f;

    bool hasCross = (ch.style == CrosshairConfig::Cross || ch.style == CrosshairConfig::CrossDot);
    bool hasDot = (ch.style == CrosshairConfig::Dot || ch.style == CrosshairConfig::CrossDot);
    bool hasCircle = (ch.style == CrosshairConfig::Circle);

    auto drawCrossArms = [&](float expand, SkPaint& paint) {
        float e = expand;
        // Right
        canvas->drawRect(SkRect::MakeXYWH(cx + ch.currentSpread - e, cy - ht - e,
                         ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e), paint);
        // Left
        canvas->drawRect(SkRect::MakeXYWH(cx - ch.size - e, cy - ht - e,
                         ch.size - ch.currentSpread + 2*e, ch.thickness + 2*e), paint);
        // Bottom
        canvas->drawRect(SkRect::MakeXYWH(cx - ht - e, cy + ch.currentSpread - e,
                         ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e), paint);
        // Top
        canvas->drawRect(SkRect::MakeXYWH(cx - ht - e, cy - ch.size - e,
                         ch.thickness + 2*e, ch.size - ch.currentSpread + 2*e), paint);
    };

    // Outline
    if (ch.outline) {
        SkPaint outPaint;
        outPaint.setAntiAlias(true);
        outPaint.setColor(SkColorSetARGB(ch.outA, ch.outR, ch.outG, ch.outB));
        if (hasCross) drawCrossArms(ot, outPaint);
        if (hasDot) {
            outPaint.setStyle(SkPaint::kFill_Style);
            canvas->drawCircle(cx, cy, ch.dotSize + ot, outPaint);
        }
        if (hasCircle) {
            outPaint.setStyle(SkPaint::kStroke_Style);
            outPaint.setStrokeWidth(ch.thickness + 2 * ot);
            canvas->drawCircle(cx, cy, ch.size, outPaint);
        }
    }

    // Fill
    SkPaint fillPaint;
    fillPaint.setAntiAlias(true);
    fillPaint.setColor(SkColorSetARGB(ch.a, ch.r, ch.g, ch.b));
    if (hasCross) drawCrossArms(0, fillPaint);
    if (hasDot) {
        fillPaint.setStyle(SkPaint::kFill_Style);
        canvas->drawCircle(cx, cy, ch.dotSize, fillPaint);
    }
    if (hasCircle) {
        fillPaint.setStyle(SkPaint::kStroke_Style);
        fillPaint.setStrokeWidth(ch.thickness);
        canvas->drawCircle(cx, cy, ch.size, fillPaint);
    }
}

Engine::~Engine() {
    // Ensure layout thread is stopped (safety — normally joined in run())
    if (layoutThread_.joinable()) {
        layoutShared_.state.store(kLayoutShutdown, std::memory_order_release);
        layoutShared_.state.notify_one();
        layoutThread_.join();
    }
    // Ensure raster thread is stopped (safety — normally joined in run())
    if (rasterThread_.joinable()) {
        rasterShared_.state.store(kRasterShutdown, std::memory_order_release);
        rasterShared_.state.notify_one();
        rasterThread_.join();
    }
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

    // Canvas threads are stopped by ~CanvasScene (unique_ptr destruction)
    canvasScenes_.clear();

    // WebGL contexts (unique_ptr destruction handles cleanup)
    webglEntries_.clear();
    canvasScenes_.clear();
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
        // Delete document BEFORE elem_map — the map holds JS refs to elements
        // whose finalizers call freeNode(); deleting the map first can free
        // elements that are still referenced by the document tree, causing
        // use-after-free when the document wrapper is subsequently collected.
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
    // Destroy JS runtime BEFORE document — JS_FreeRuntime() runs GC finalizers
    // that dereference Element pointers, so elements must still be alive.
    // Audio engine must also outlive JS runtime because VoiceAllocator/MidiInput
    // destructors reference it (removeVoice, close).
    jsRuntime_.reset();
    physicsWorld_.reset();
    audioEngine_.reset();
    document_.reset();
    timers_.reset();
    renderer_.reset();
}

// ---------------------------------------------------------------------------
// Lightweight tick for use during modal blocking (move/resize, dialogs)
// ---------------------------------------------------------------------------

void Engine::tickTimersOnly()
{
    double now = util::currentTimeMs();
    timers_->tick(now);
    jsRuntime_->executePendingJobs();
}

/// SDL event watcher — fires on the main thread during Windows' modal
/// move/resize loop, keeping JS timers (audio sequencer etc.) alive.
static bool modalEventWatcher(void* userdata, SDL_Event* event)
{
    if (event->type >= SDL_EVENT_WINDOW_FIRST &&
        event->type <= SDL_EVENT_WINDOW_LAST) {
        static_cast<Engine*>(userdata)->tickTimersOnly();
    }
    return true; // keep the event in the queue
}

// ---------------------------------------------------------------------------
// Layout thread — owns style resolution + layout computation.
// Reads DOM tree (read-only after JS phase), writes computedStyle_ and
// layoutBox_ on each element. No GL needed — uses CPU-only RasterRenderer
// for text measurement.
// ---------------------------------------------------------------------------

void Engine::layoutThreadFunc() {
    // Thread-local renderer for text measurement (CPU-only, no GL required)
    auto layoutRenderer = std::make_unique<render::RasterRenderer>();

    // Register custom fonts on the layout renderer so text measurement uses them
    for (auto& font : loadedFonts_) {
        layoutRenderer->registerCustomFont(font.family, font.data.data(),
                                           font.data.size(), font.weight, font.italic);
    }

    layout::FontManager layoutFontManager;
    layout::SkiaTextMetrics layoutTextMetrics(layoutRenderer.get(), &layoutFontManager);

    LOG_INFO("Layout thread started");

    while (true) {
        layoutShared_.state.wait(kLayoutIdle, std::memory_order_acquire);
        uint32_t s = layoutShared_.state.load(std::memory_order_acquire);

        if (s == kLayoutShutdown) break;
        if (s != kLayoutDomStable) continue;

        layoutShared_.state.store(kLayoutBusy, std::memory_order_relaxed);

        int vpW = layoutShared_.vpWidth.load(std::memory_order_relaxed);
        int vpH = layoutShared_.vpHeight.load(std::memory_order_relaxed);

        // Style resolution + layout computation
        if (document_) {
            layout::ElementRefAdapter::setHoveredElement(
                layoutShared_.hoveredElement.load(std::memory_order_relaxed));
            double now = util::currentTimeMs();
            document_->setTransitionManager(&transitionManager_, now);
            auto& kfs = document_->cascade().keyframes();
            animationManager_.setKeyframes(&kfs);
            document_->setAnimationManager(&animationManager_);
            document_->resolveStyles();
            // If transitions or animations are active, keep re-rendering
            bool animActive = transitionManager_.tick(now) | animationManager_.tick(now);
            layoutShared_.animationsActive.store(animActive, std::memory_order_relaxed);
            if (animActive) {
                document_->markDirty();
            }
            // performLayout() rebuilds the persistent layout tree when
            // structureDirty_ is set and clears the flag itself.
            int insetTop = layoutShared_.insetTop.load(std::memory_order_relaxed);
            document_->performLayout(static_cast<float>(vpW),
                                     static_cast<float>(vpH - insetTop),
                                     layoutTextMetrics);
            document_->clearDirty();
        }

        // Transition to done — use compare_exchange to avoid overwriting
        // a pending kLayoutShutdown signal from the main thread.
        uint32_t expected = kLayoutBusy;
        if (layoutShared_.state.compare_exchange_strong(
                expected, kLayoutDone,
                std::memory_order_release, std::memory_order_acquire)) {
            layoutShared_.state.notify_one();
        } else {
            // State was changed (shutdown) — exit
            break;
        }
    }

    layoutRenderer.reset();
    LOG_INFO("Layout thread stopped");
}

// ---------------------------------------------------------------------------
// Raster thread — owns HTML draw traversal + GPU surface pool.
// Reads DOM layout data (read-only after main thread layout completes),
// produces GPU textures, signals main thread via atomic state + GL fence.
// ---------------------------------------------------------------------------

void Engine::rasterThreadFunc() {
    // Main thread has already created rasterGLContext_ (macOS/AppKit
    // requirement); we only MakeCurrent it here, which is a thread-local
    // GL operation. Main is parked in run() on rasterReady_ so no other
    // wgl*Context call can overlap with this one (Windows/NVIDIA requirement).
    SDL_GL_MakeCurrent(window_->getSDLWindow(), rasterGLContext_);
    rasterReady_.store(true, std::memory_order_release);
    rasterReady_.notify_one();

    // Create a raster-thread-local SkiaRenderer (creates its own GrDirectContext
    // internally — each thread needs its own since Skia GPU contexts aren't thread-safe).
    // The GLContext reference is shared for texture/FBO helper methods only.
    auto rasterRenderer = std::make_unique<render::SkiaRenderer>(*gl_);
    if (!rasterRenderer->grContext()) {
        LOG_ERROR("Raster thread: SkiaRenderer failed to create GrDirectContext");
        return;
    }
    // Register custom fonts on the raster renderer
    for (auto& font : loadedFonts_) {
        rasterRenderer->registerCustomFont(font.family, font.data.data(),
                                           font.data.size(), font.weight, font.italic);
    }

    // Separate FontManager so font handles are created against the raster renderer.
    // The shared FontManager caches handles for the main thread's renderer — those
    // handles are invalid on the raster thread's SkiaRenderer.
    layout::FontManager rasterFontManager;
    auto rasterDrawTraversal = std::make_unique<layout::DrawTraversal>(
        rasterRenderer.get(), &rasterFontManager);

    LOG_INFO("Raster thread started");

    while (true) {
        // Wait for work (C++20 atomic wait — futex, not a mutex)
        rasterShared_.state.wait(kRasterIdle, std::memory_order_acquire);
        uint32_t s = rasterShared_.state.load(std::memory_order_acquire);

        if (s == kRasterShutdown) break;
        if (s != kRasterDomStable) continue;

        rasterShared_.state.store(kRasterBusy, std::memory_order_relaxed);

        // Read snapshot values from main thread
        int vpW = rasterShared_.vpWidth.load(std::memory_order_relaxed);
        int vpH = rasterShared_.vpHeight.load(std::memory_order_relaxed);
        int insetTop = rasterShared_.insetTop.load(std::memory_order_relaxed);
        int contentH = vpH - insetTop;
        float scrollY = std::bit_cast<float>(
            rasterShared_.scrollYBits.load(std::memory_order_relaxed));

        // Determine which layer buffer to write to (back buffer)
        int backIdx = 1 - rasterShared_.frontBuffer.load(std::memory_order_relaxed);
        auto& backBuf = layerBuffers_[backIdx];
        backBuf.appLayers.clear();
        backBuf.systemLayers.clear();

        // Reset Ganesh GL state tracking for this frame
        rasterRenderer->grContext()->resetContext();

        // Invalidate surface pool on viewport resize
        if (htmlSurfacePoolW_ != vpW || htmlSurfacePoolH_ != vpH) {
            for (auto& ps : htmlSurfacePool_) {
                rasterRenderer->destroyGPUSurface(ps);
            }
            htmlSurfacePool_.clear();
            htmlSurfacePoolW_ = vpW;
            htmlSurfacePoolH_ = vpH;
        }

        int htmlLayerIdx = 0;

        // Set up layer break callback for canvas/WebGL elements
        rasterDrawTraversal->setLayerBreakCallback(
            [this, &rasterRenderer, &htmlLayerIdx, &backBuf, vpW, vpH](
                canvas::CanvasScene* scene, unsigned int directTexture,
                float x, float y, float w, float h) {
                int prevIdx = htmlLayerIdx;
                htmlLayerIdx++;
                while (htmlLayerIdx >= static_cast<int>(htmlSurfacePool_.size())) {
                    htmlSurfacePool_.push_back(
                        rasterRenderer->createGPUSurface(vpW, vpH));
                }
                rasterRenderer->switchSurface(htmlSurfacePool_[htmlLayerIdx].surface);

                UILayer htmlLayer;
                htmlLayer.type = UILayer::HTML;
                htmlLayer.texture = htmlSurfacePool_[prevIdx].texture;
                backBuf.appLayers.push_back(std::move(htmlLayer));

                UILayer canvasLayer;
                canvasLayer.type = UILayer::Canvas;
                canvasLayer.canvasScene = scene;
                canvasLayer.texture = directTexture;  // non-zero for WebGL
                canvasLayer.cx = x; canvasLayer.cy = y;
                canvasLayer.cw = w; canvasLayer.ch = h;
                backBuf.appLayers.push_back(std::move(canvasLayer));
            });

        // Begin frame
        rasterRenderer->beginFrame(vpW, vpH);

        // Ensure pool has a GPU surface for HTML layer 0
        if (htmlSurfacePool_.empty()) {
            htmlSurfacePool_.push_back(rasterRenderer->createGPUSurface(vpW, vpH));
        }
        // Rewrap existing pool surfaces with fresh Skia wrappers
        for (auto& ps : htmlSurfacePool_) {
            rasterRenderer->rewrapGPUSurface(ps, vpW, vpH);
        }
        // Switch to pool surface for HTML layer 0
        auto origSurface = rasterRenderer->switchSurface(htmlSurfacePool_[0].surface);

        // Draw traversal — reads layout boxes and computed styles (read-only).
        // App content is translated down by insetTop so the top strip is
        // reserved for the engine-owned menu bar.
        if (document_ && document_->documentElement()) {
            // Resolve app-relative background-image URLs against the app dir.
            // (System panels below set their own basePath; we re-establish this
            // each frame because that call mutates shared state.)
            rasterDrawTraversal->setBasePath(document_->basePath());
            rasterDrawTraversal->draw(document_->documentElement(),
                                      0, static_cast<float>(insetTop) - scrollY,
                                      vpW, contentH, insetTop);

            // Selection highlight overlay: drawn after the HTML so it sits on
            // top of text. Semi-transparent accent color. Only fires when
            // there's a non-empty selection in the app document.
            drawSelectionHighlight(rasterRenderer.get(),
                                   static_cast<float>(insetTop) - scrollY);
        }

        // Draw the active app-context overlay (dropdown / color picker / etc.)
        // on top of all elements.
        overlayMgr_.drawIfContext(OverlayContext::App, rasterRenderer.get());

        // Draw viewport scrollbar in the content area below the menu bar
        {
            float ct = static_cast<float>(insetTop);
            float vh = static_cast<float>(contentH);
            auto& vs = viewportScrollbar_.style();
            auto m = viewportScrollbar_.layout(
                static_cast<float>(vpW) - vs.width - vs.margin,
                ct, vh, documentHeight_, vh, scrollY);
            viewportScrollbar_.draw(rasterRenderer.get(), m);
        }

        // Draw scrollbars for overflow elements in the app document. System
        // panels draw their own scrollbars via drawElementScrollbars from
        // drawSystemPanels (same helper, different tree).
        if (document_) {
            drawElementScrollbars(rasterRenderer.get(),
                                  document_->documentElement(),
                                  0.0f, static_cast<float>(insetTop) - scrollY);
        }

        // Capture the last HTML layer
        rasterRenderer->switchSurface(origSurface);
        UILayer lastHtml;
        lastHtml.type = UILayer::HTML;
        lastHtml.texture = htmlSurfacePool_[htmlLayerIdx].texture;
        backBuf.appLayers.push_back(std::move(lastHtml));

        // Flush each pool surface's deferred Ganesh ops
        for (int i = 0; i <= htmlLayerIdx; ++i) {
            if (htmlSurfacePool_[i].surface && rasterRenderer->grContext()) {
                rasterRenderer->grContext()->flush(htmlSurfacePool_[i].surface.get());
            }
        }
        rasterDrawTraversal->setLayerBreakCallback(nullptr);

        // --- System panels ---
        // Lay out and draw each visible system panel into its own GPU-backed
        // Skia surface. These composite on top of the crosshair on the main
        // thread via backBuf.systemLayers. Layout runs here on the raster
        // thread too — safe because system DOM mutations happen on the main
        // thread during the JS phase, before we're signaled.
        if (isSystemVisible()) {
            // Resize system surface pool on viewport change
            if (systemSurfacePoolW_ != vpW || systemSurfacePoolH_ != vpH) {
                for (auto& ps : systemSurfacePool_) {
                    rasterRenderer->destroyGPUSurface(ps);
                }
                systemSurfacePool_.clear();
                systemSurfacePoolW_ = vpW;
                systemSurfacePoolH_ = vpH;
            }

            layout::SkiaTextMetrics sysMetrics(rasterRenderer.get(),
                                               &rasterFontManager);
            layoutSystemPanels(sysMetrics);

            size_t panelIdx = 0;
            for (auto& sdoc : systemDocs_) {
                if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;

                while (panelIdx >= systemSurfacePool_.size()) {
                    systemSurfacePool_.push_back(
                        rasterRenderer->createGPUSurface(vpW, vpH));
                }
                rasterRenderer->rewrapGPUSurface(systemSurfacePool_[panelIdx], vpW, vpH);
                rasterRenderer->switchSurface(systemSurfacePool_[panelIdx].surface);

                // Shared per-doc draw — installs canvas-blit callback, runs
                // the layout traversal, and draws overflow scrollbars. Same
                // helper used by the headless path so decoration passes never
                // have to be remembered in two places.
                drawSystemPanelDoc(rasterRenderer.get(), *rasterDrawTraversal,
                                   sdoc, vpW, vpH);

                UILayer panelLayer;
                panelLayer.type = UILayer::HTML;
                panelLayer.texture = systemSurfacePool_[panelIdx].texture;
                backBuf.systemLayers.push_back(std::move(panelLayer));

                if (rasterRenderer->grContext()) {
                    rasterRenderer->grContext()->flush(
                        systemSurfacePool_[panelIdx].surface.get());
                }
                panelIdx++;
            }
            rasterRenderer->switchSurface(origSurface);
            systemDirty_ = false;
        }

        rasterRenderer->endFrame();

        // GL fence sync — ensures all GPU commands complete before main thread
        // samples the textures for compositing.
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();  // ensure fence is submitted to GPU command stream

        // Publish results: fence, layer buffer, state.
        // Use compare_exchange to avoid overwriting a pending kRasterShutdown.
        rasterShared_.fenceSync.store(reinterpret_cast<uintptr_t>(fence),
                                       std::memory_order_relaxed);
        rasterShared_.frontBuffer.store(backIdx, std::memory_order_relaxed);
        uint32_t expected = kRasterBusy;
        if (rasterShared_.state.compare_exchange_strong(
                expected, kRasterTexturesReady,
                std::memory_order_release, std::memory_order_acquire)) {
            rasterShared_.state.notify_one();
        } else {
            // Shutdown requested while busy — clean up fence and exit
            glDeleteSync(fence);
            break;
        }
    }

    // Cleanup
    for (auto& ps : htmlSurfacePool_) {
        rasterRenderer->destroyGPUSurface(ps);
    }
    htmlSurfacePool_.clear();
    for (auto& ps : systemSurfacePool_) {
        rasterRenderer->destroyGPUSurface(ps);
    }
    systemSurfacePool_.clear();
    // SkiaRenderer destructor handles GrContext cleanup
    rasterRenderer.reset();
    SDL_GL_MakeCurrent(window_->getSDLWindow(), nullptr);
    LOG_INFO("Raster thread stopped");
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

double Engine::serverUptime() const {
    if (serverStartTime_ <= 0.0) return 0.0;
    return (util::currentTimeMs() - serverStartTime_) / 1000.0;
}

void Engine::run() {
    // Headless mode: initial layout is done in constructor, return immediately.
    // The HeadlessController drives subsequent frames via advanceTime/flush.
    if (displayMode_ == DisplayMode::Headless) {
        // Resync virtual time to current wall clock so timers registered in
        // test scripts fire correctly relative to advanceTime() calls.
        // Without this, virtualTime_ (set early in the constructor) lags behind
        // the wall clock by the time system panels and fonts finish loading.
        virtualTime_ = util::currentTimeMs();
        // Splash elapsed is measured against virtualTime_, so rebase its start
        // too — otherwise elapsed would count the constructor time and the
        // splash would auto-dismiss partway through the first advanceTime().
        if (splashVisible_) splashStartMs_ = virtualTime_;
        timers_->tick(virtualTime_);
        return;
    }

    // Server mode: tick loop driven by real wall-clock time.
    if (displayMode_ == DisplayMode::Server) {
        running_ = true;
        LOG_INFO("[server] Running at %.0f ticks/sec", serverTickRate_);

        while (running_ && !serverStopRequested_ && !bro::util::interrupted()) {
            double tickStart = util::currentTimeMs();
            double tickIntervalMs = 1000.0 / serverTickRate_;

            // 1. Tick timers
            timers_->tick(tickStart);

            // 2. Pump brokit fetch (HTTP requests)
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

            // 3. Pump brokit WebSocket
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

            // 4. Execute pending JS jobs
            jsRuntime_->executePendingJobs();

            // 5. Drain worker messages
            js::tickWorkers(jsRuntime_->getContext());
            jsRuntime_->executePendingJobs();

            // 6. Poll network (drain subscriber's event queue, fire JS callbacks)
            if (netService_) {
                js::NetBindings::poll(jsRuntime_->getContext());
                jsRuntime_->executePendingJobs();
            }

            // 7. Step physics (fixed timestep accumulator)
            if (physicsWorld_ && physicsWorld_->isIdle()) {
                physicsWorld_->consumeStep();
                double stepMs = physicsWorld_->timeStep() * 1000.0;
                double nowPhys = util::currentTimeMs();
                if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
                physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_);
                lastPhysicsTimeMs_ = nowPhys;
                if (physicsAccumMs_ >= stepMs) {
                    physicsAccumMs_ -= stepMs;
                    physicsWorld_->signalStep();
                }
            }

            // 8. Periodic GC
            double now = util::currentTimeMs();
            if (now - lastGCMs_ >= kGCIntervalMs) {
                JS_RunGC(jsRuntime_->getRuntime());
                lastGCMs_ = now;
            }

            // 9. Sleep until next tick
            double elapsed = util::currentTimeMs() - tickStart;
            double sleepMs = tickIntervalMs - elapsed;
            if (sleepMs > 0.5) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<int64_t>(sleepMs * 1000.0)));
            }
        }

        LOG_INFO("[server] Stopped (uptime: %.1fs)", serverUptime());
        return;
    }

    running_ = true;

    // Wire up event-loop callbacks.
    eventLoop_->onQuit = [this]() { running_ = false; };
    eventLoop_->onResize = [this](uint32_t w, uint32_t h) {
        handleResize(static_cast<int>(w), static_cast<int>(h));
    };
    eventLoop_->onMouseDown = [this](float x, float y, uint8_t btn) {
        handleMouseDown(x, y, static_cast<int>(btn));
    };
    eventLoop_->onMouseUp = [this](float x, float y, uint8_t btn) {
        handleMouseUp(x, y, static_cast<int>(btn));
    };
    eventLoop_->onMouseMove = [this](float x, float y, float xrel, float yrel) {
        handleMouseMove(x, y, xrel, yrel);
    };
    eventLoop_->onKeyDown = [this](int32_t keycode, int32_t scancode, uint16_t mod, bool repeat) {
        handleKeyDown(keycode, scancode, static_cast<int>(mod), repeat);
    };
    eventLoop_->onKeyUp = [this](int32_t keycode, int32_t scancode, uint16_t mod, bool repeat) {
        handleKeyUp(keycode, scancode, static_cast<int>(mod), repeat);
    };
    eventLoop_->onTextInput = [this](const std::string& text) {
        handleTextInput(text);
    };
    eventLoop_->onWheel = [this](float x, float y, float dx, float dy) {
        handleWheel(x, y, dx, dy);
    };

    eventLoop_->onDropFile = [this](const std::string& path, float x, float y) {
        handleDropFile(path, x, y);
    };
    eventLoop_->onDropText = [this](const std::string& text, float x, float y) {
        handleDropText(text, x, y);
    };
    // SDL drops relative mouse mode on focus loss on some platforms — keep our
    // engine-side lock state in sync so apps see a pointerlockchange.
    eventLoop_->onFocusLost = [this]() {
        exitPointerLock();
        setPageVisibility(false);
    };
    eventLoop_->onFocusGained = [this]() {
        setPageVisibility(true);
    };

    // Initial layout
    if (document_) {
        ensureReplacedElements(document_->documentElement());
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_);
        double now = util::currentTimeMs();
        document_->setTransitionManager(&transitionManager_, now);
        animationManager_.setKeyframes(&document_->cascade().keyframes());
        document_->setAnimationManager(&animationManager_);
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(viewportWidth_),
                                 static_cast<float>(contentHeight()), *textMetrics_);
        if (document_->documentElement()) {
            auto& box = document_->documentElement()->layoutBox();
            documentHeight_ = box.marginBox().height;
        }
    }

    auto* skia = static_cast<render::SkiaRenderer*>(renderer_.get());

    // Start canvas threads for any existing canvas scenes that weren't
    // threaded at addCanvasScene time. Main thread creates each shared
    // context; startThread blocks until the worker MakeCurrents it.
    for (auto& cs : canvasScenes_) {
        if (cs && !cs->isThreaded()) {
            auto ctx = window_->createSharedContext();
            if (ctx) cs->startThread(ctx, window_->getSDLWindow());
        }
    }

    // Raster thread: create its shared GL context on the main thread
    // (macOS/AppKit requirement), then block until the worker has
    // MakeCurrent'd it so no later wgl*Context call can overlap.
    rasterGLContext_ = window_->createSharedContext();
    if (!rasterGLContext_) {
        LOG_ERROR("Failed to create shared GL context for raster thread");
        return;
    }
    rasterReady_.store(false, std::memory_order_relaxed);

    // Launch layout thread (style resolution + layout computation)
    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);

    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);

    // Event watcher keeps JS timers alive during Windows' modal move/resize loop.
    SDL_AddEventWatch(modalEventWatcher, this);

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        double frameStart = util::currentTimeMs();

        // 0. Ensure layout thread is not running before we process events.
        //    Event handlers run JS that can mutate the DOM (setAttribute,
        //    classList.toggle, etc.) — those writes race with the layout
        //    thread's resolveStyles() which reads element attributes.
        {
            uint32_t ls = layoutShared_.state.load(std::memory_order_acquire);
            if (ls == kLayoutBusy || ls == kLayoutDomStable) {
                layoutShared_.state.wait(kLayoutBusy, std::memory_order_acquire);
                while (layoutShared_.state.load(std::memory_order_acquire) == kLayoutDomStable)
                    layoutShared_.state.wait(kLayoutDomStable, std::memory_order_acquire);
                ls = layoutShared_.state.load(std::memory_order_acquire);
            }
            if (ls == kLayoutDone) {
                layoutShared_.state.store(kLayoutIdle, std::memory_order_release);
                if (document_ && document_->documentElement()) {
                    auto& box = document_->documentElement()->layoutBox();
                    documentHeight_ = box.marginBox().height;
                }
                // Also drain here (early layout completion from previous frame)
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
            }
        }

        // Pump HTMLMediaElement events on every <video> — must happen on
        // the main thread since QuickJS isn't thread-safe.
        pumpVideoEvents();

        // 1. Poll platform events
        eventLoop_->pollEvents();
        if (eventLoop_->shouldQuit()) {
            running_ = false;
            break;
        }

        // 1b. Consume physics step from previous frame (makes new positions available to JS).
        if (physicsWorld_) {
            physicsWorld_->consumeStep();
        }

        // 1c. Prune detached scene graphs (elements removed from DOM).
        sceneGraphs_.erase(
            std::remove_if(sceneGraphs_.begin(), sceneGraphs_.end(),
                [](auto& sg) {
                    if (!sg.element) return false;
                    auto* n = sg.element;
                    while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                    return n->tagName() != "html" && n->tagName() != "HTML";
                }),
            sceneGraphs_.end());

        // Sync scene graph physics nodes (body transforms → node transforms).
        for (auto& sg : sceneGraphs_) {
            sg.graph->syncPhysics();
        }

        // 1d. Sync scene graph AI bindings (world.tick, per-agent think, transform write).
        //     Uses real frame dt; the AIWorldTicker internally steps world at a fixed rate.
        {
            double nowMs = util::currentTimeMs();
            float frameDt = (lastFrameTimeMs_ > 0.0)
                ? static_cast<float>((nowMs - lastFrameTimeMs_) / 1000.0)
                : 1.0f / 60.0f;
            if (frameDt < 0.0f) frameDt = 0.0f;
            if (frameDt > 0.1f) frameDt = 0.1f; // clamp on long stalls
            lastFrameTimeMs_ = nowMs;
            for (auto& sg : sceneGraphs_) {
                sg.graph->syncAgents(frameDt);
            }
            drainWheelSmoothing(frameDt);
        }

        // 2. Tick timers + JS execution
        double now = util::currentTimeMs();
        double t0 = now;
        timers_->tick(now);

        // 2a. Tick brokit fetch (pump pending HTTP requests)
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

        // 2b. Tick brokit WebSocket (pump pending connections/messages)
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

        // 2c. Tick system panel timers
        tickSystemPanels(now);
        // System panels (splash animation, menu, perf, settings) now share the
        // raster thread, which is signaled via uiDirty_. Their own DOM edits
        // never touch the app document, so propagate systemDirty_ so the
        // raster thread actually gets kicked each frame the splash animates.
        if (systemDirty_) uiDirty_ = true;

        // 3. Bind WebGL FBO before JS callbacks (so gl.bindFramebuffer(null) targets canvas)
        //    Also resize WebGL FBO to match element layout if needed.
        webgl::WebGL2RenderingContext* activeWebGL = nullptr;
        if (!webglEntries_.empty()) {
            auto& entry = webglEntries_[0];
            activeWebGL = entry.context.get();
            if (entry.element) {
                auto& box = entry.element->layoutBox();
                int ew = static_cast<int>(box.contentRect.width);
                int eh = static_cast<int>(box.contentRect.height);
                if (ew > 0 && eh > 0 &&
                    (ew != activeWebGL->canvasWidth() || eh != activeWebGL->canvasHeight())) {
                    activeWebGL->resize(ew, eh);
                }
            }
            activeWebGL->bindCanvasFBO();
        }

        // 3a. Fire requestAnimationFrame callbacks
        timers_->fireAnimationFrames(now);

        double tGlSave = util::currentTimeMs();

        // 3b. Run pending JS jobs (promises, etc.)
        jsRuntime_->executePendingJobs();

        // 3b2. Drain worker→main messages (calls onmessage callbacks)
        js::tickWorkers(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();

        // 3b3. Poll network (drain subscriber's event queue, fire JS callbacks)
        if (netService_) {
            js::NetBindings::poll(jsRuntime_->getContext());
            jsRuntime_->executePendingJobs();
        }

        // 3c. Unbind WebGL FBO
        if (activeWebGL) {
            activeWebGL->unbindCanvasFBO();
        }

        // 3c1. Materialize dirty HtmlNodes on the main thread (layout + paint
        //       + GL upload). Runs here — not on the raster thread — so that
        //       JS mutations to each HtmlNode's detached Document stay on the
        //       same thread that reads it during style resolution + layout.
        if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) sg.graph->materializeHtmlNodes(skia, &fontManager_);
            }
        }

        // 3c2. Auto-render scene graphs (after JS has updated transforms/camera).
        //       Resize to match element layout if needed (mirrors WebGL resize above).
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
            sg.graph->render();
        }

        double tJs = util::currentTimeMs();
        accumJsMs_ += tJs - t0;
        accumGlStateMs_ += tJs - tGlSave;  // GL save is inside JS phase

        // 3d. Periodic QuickJS cycle-collector GC + orphan sweep
        if (now - lastGCMs_ >= kGCIntervalMs) {
            js::DomBindings::sweepOrphanedWrappers(jsRuntime_->getContext());
            JS_RunGC(jsRuntime_->getRuntime());
            lastGCMs_ = now;
        }

        // 3e. Signal physics thread at fixed rate (not every frame).
        if (physicsWorld_ && physicsWorld_->isIdle()) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            double nowPhys = util::currentTimeMs();
            if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
            physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_);
            lastPhysicsTimeMs_ = nowPhys;
            if (physicsAccumMs_ >= stepMs) {
                physicsAccumMs_ -= stepMs;
                if (physicsAccumMs_ > stepMs * 3)
                    physicsAccumMs_ = 0;
                physicsWorld_->signalStep();
            }
        }

        // 4. Signal layout thread when DOM is dirty.
        //    Layout thread runs resolveStyles + performLayout on its own thread.
        //    Only signal when layout thread idle AND raster thread idle — ensures
        //    no overlap between layout writes, raster reads, and JS mutations.
        //    ensureReplacedElements must run on main thread before layout (needs renderer).

        double tLayout = tJs;
        bool layoutIdle = (layoutShared_.state.load(std::memory_order_acquire) == kLayoutIdle);
        bool rasterIdle = (rasterShared_.state.load(std::memory_order_acquire) == kRasterIdle);
        bool layoutSignaled = false;

        bool animActive = layoutShared_.animationsActive.load(std::memory_order_relaxed);
        // Scene-graph HtmlNodes own detached DOM trees that the layout/raster
        // pipeline doesn't otherwise see. Force a pass whenever any is dirty
        // so imperative JS edits via node.root actually re-rasterize.
        bool sceneHtmlDirty = false;
        for (auto& sg : sceneGraphs_) {
            if (sg.graph && sg.graph->hasPendingHtmlWork()) { sceneHtmlDirty = true; break; }
        }
        if (layoutIdle && rasterIdle && document_ && (document_->isDirty() || animActive || sceneHtmlDirty || !hasRenderedOnce_)) {
            if (document_->isStructureDirty()) {
                ensureReplacedElements(document_->documentElement());
            }
            layoutShared_.vpWidth.store(viewportWidth_, std::memory_order_relaxed);
            layoutShared_.vpHeight.store(viewportHeight_, std::memory_order_relaxed);
            layoutShared_.insetTop.store(contentTop(), std::memory_order_relaxed);
            layoutShared_.hoveredElement.store(hoveredElement_, std::memory_order_relaxed);
            layoutShared_.state.store(kLayoutDomStable, std::memory_order_release);
            layoutShared_.state.notify_one();
            layoutSignaled = true;
        }
        accumLayoutMs_ += util::currentTimeMs() - tJs;

        // === GPU FRAME (threaded rasterization + main-thread compositing) ===
        // Layout thread runs in parallel with composite + swap below.

        // 5a. Signal canvas threads (each has its own GL context + GrDirectContext).
        //     Done every frame since canvases animate independently of HTML layout.
        double tRaster = util::currentTimeMs();
        int front = rasterShared_.frontBuffer.load(std::memory_order_acquire);
        auto& frontLayers = layerBuffers_[front].appLayers;
        for (auto& layer : frontLayers) {
            if (layer.type == UILayer::Canvas && layer.canvasScene) {
                layer.canvasScene->prepareAndSignal();
            }
        }
        tRaster = util::currentTimeMs();

        // 5b. Check if raster thread has new textures ready.
        //     If so, wait on GL fence (GPU-side wait) and transition back to idle.
        if (rasterShared_.state.load(std::memory_order_acquire) == kRasterTexturesReady) {
            auto fence = reinterpret_cast<GLsync>(
                rasterShared_.fenceSync.exchange(0, std::memory_order_relaxed));
            if (fence) {
                glWaitSync(fence, 0, GL_TIMEOUT_IGNORED);
                glDeleteSync(fence);
            }
            // Re-read front buffer (raster thread may have flipped it)
            front = rasterShared_.frontBuffer.load(std::memory_order_acquire);
            rasterShared_.state.store(kRasterIdle, std::memory_order_release);
            rasterIdle = true;
        }

        // 5b2. Wait for canvas thread fences before compositing.
        for (auto& layer : frontLayers) {
            if (layer.type == UILayer::Canvas && layer.canvasScene) {
                layer.canvasScene->consumeFence();
            }
        }
        accumRasterMs_ += util::currentTimeMs() - tRaster;

        double tGpu = util::currentTimeMs();

        // 5d. Update canvas scene scroll + clean up detached
        for (auto& cs : canvasScenes_) {
            cs->setViewportScroll(scrollY_);
            cs->checkDetached();
        }
        canvasScenes_.erase(
            std::remove_if(canvasScenes_.begin(), canvasScenes_.end(),
                [](auto& cs) { return cs->isDetached(); }),
            canvasScenes_.end());

        // 5e. Set viewport and clear
        glViewport(0, 0, viewportWidth_, viewportHeight_);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        // 5f. Composite app UI layers in DOM order
        //     HTML layers (cached textures from raster thread) interleaved with
        //     canvas layers (freshly rasterized on main thread).
        compositeLayers(layerBuffers_[front].appLayers);

        // 5g. Tick + draw crosshair overlay (runs at full frame rate, after app content)
        crosshair_.tick(static_cast<float>(totalFrameMs_ * 0.001));
        drawCrosshairGL();

        // 5h. Composite system panel layers (menu bar / preferences / splash)
        //     on top of crosshair. Each entry is a GPU-backed Skia surface
        //     produced by the raster thread — same pipeline as app layers.
        compositeLayers(layerBuffers_[front].systemLayers);

        // Restore WebGL shadow state so apps with internal caches (three.js)
        // see the same GL state they left on the previous frame.
        if (activeWebGL) {
            activeWebGL->restoreState();
        }

        // Measure GPU work before swap (swap includes vsync wait)
        accumGpuMs_ += util::currentTimeMs() - tGpu;

        // Swap buffers (may block on vsync — not counted as GPU work)
        gl_->swapBuffers();

        // 5j. Wait for layout thread and consume results.
        //     Layout ran in parallel with composite+swap above.
        if (layoutSignaled) {
            // Wait for layout to finish (blocking — but it overlapped with composite)
            uint32_t ls = layoutShared_.state.load(std::memory_order_acquire);
            if (ls == kLayoutBusy || ls == kLayoutDomStable) {
                layoutShared_.state.wait(kLayoutBusy, std::memory_order_acquire);
                while (layoutShared_.state.load(std::memory_order_acquire) == kLayoutDomStable) {
                    layoutShared_.state.wait(kLayoutDomStable, std::memory_order_acquire);
                }
                ls = layoutShared_.state.load(std::memory_order_acquire);
            }

            if (ls == kLayoutDone) {
                layoutShared_.state.store(kLayoutIdle, std::memory_order_release);

                // Update document height for scroll clamping
                if (document_ && document_->documentElement()) {
                    auto& box = document_->documentElement()->layoutBox();
                    documentHeight_ = box.marginBox().height;
                }

                // Process auto-scroll-to-bottom for tracked overflow elements.
                if (document_ && !document_->scrollToBottomElements().empty()) {
                    auto pending = document_->scrollToBottomElements();
                    for (auto* elem : pending) {
                        std::string ov = getOverflowY(elem->computedStyle());
                        if (overflowClips(ov)) {
                            elem->setScrollTopValue(maxScrollTop(elem));
                        }
                        elem->setScrollToBottom(false);
                    }
                }

                // Notify ResizeObserver / IntersectionObserver after layout
                if (jsRuntime_) {
                    auto* ctx = jsRuntime_->getContext();
                    if (JS_IsUndefined(observerCheckFn_)) {
                        observerCheckFn_ = JS_Eval(ctx, js_observer_check,
                                                   strlen(js_observer_check),
                                                   "<observer-check>",
                                                   JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
                    }
                    if (!JS_IsUndefined(observerCheckFn_) && !JS_IsException(observerCheckFn_)) {
                        JSValue r = JS_EvalFunction(ctx, JS_DupValue(ctx, observerCheckFn_));
                        JS_FreeValue(ctx, r);
                    }
                }

                // Drain CSS transition/animation events on the main thread.
                {
                    auto te = transitionManager_.takePendingEvents();
                    auto ae = animationManager_.takePendingEvents();
                    for (auto& ev : te) {
                        dom::TransitionEvent tevt(ev.type, true, false);
                        tevt.setPropertyName(ev.name);
                        tevt.setElapsedTime(ev.elapsedTime);
                        tevt.setIsTrusted(true);
                        dispatchEvent(ev.element, tevt);
                    }
                    for (auto& ev : ae) {
                        dom::AnimationEvent aevt(ev.type, true, false);
                        aevt.setAnimationName(ev.name);
                        aevt.setElapsedTime(ev.elapsedTime);
                        aevt.setIsTrusted(true);
                        dispatchEvent(ev.element, aevt);
                    }
                }

                // Flush microtasks from event handlers (may have queued DOM mutations)
                jsRuntime_->executePendingJobs();

                uiDirty_ = true;

                // Signal raster thread now that layout is complete.
                rasterIdle = (rasterShared_.state.load(std::memory_order_acquire) == kRasterIdle);
                bool uiThrottled = (now - lastUIRenderMs_ < uiFrameIntervalMs_);
                if (rasterIdle && !uiThrottled) {
                    stageSystemPanelCanvases();
                    rasterShared_.vpWidth.store(viewportWidth_, std::memory_order_relaxed);
                    rasterShared_.vpHeight.store(viewportHeight_, std::memory_order_relaxed);
                    rasterShared_.insetTop.store(contentTop(), std::memory_order_relaxed);
                    rasterShared_.scrollYBits.store(std::bit_cast<uint32_t>(scrollY_),
                                                     std::memory_order_relaxed);
                    rasterShared_.state.store(kRasterDomStable, std::memory_order_release);
                    rasterShared_.state.notify_one();
                    uiDirty_ = false;
                    hasRenderedOnce_ = true;
                    lastUIRenderMs_ = now;
                }
            }
        } else if (rasterIdle) {
            // No layout this frame — signal raster directly if dirty.
            bool uiThrottled = (now - lastUIRenderMs_ < uiFrameIntervalMs_);
            if ((uiDirty_ || !hasRenderedOnce_) && !uiThrottled) {
                stageSystemPanelCanvases();
                rasterShared_.vpWidth.store(viewportWidth_, std::memory_order_relaxed);
                rasterShared_.vpHeight.store(viewportHeight_, std::memory_order_relaxed);
                rasterShared_.scrollYBits.store(std::bit_cast<uint32_t>(scrollY_),
                                                 std::memory_order_relaxed);
                rasterShared_.state.store(kRasterDomStable, std::memory_order_release);
                rasterShared_.state.notify_one();
                uiDirty_ = false;
                hasRenderedOnce_ = true;
                lastUIRenderMs_ = now;
            }
        }

        // 6. Frame time tracking
        totalFrameMs_ = util::currentTimeMs() - frameStart;
        double totalFrameMs = totalFrameMs_;
        statsAccumMs_ += totalFrameMs;
        statsFrameCount_++;
        if (totalFrameMs < statsMinFrameMs_) statsMinFrameMs_ = totalFrameMs;
        if (totalFrameMs > statsMaxFrameMs_) statsMaxFrameMs_ = totalFrameMs;
        if (statsAccumMs_ >= 500.0) {
            statsFps_ = statsFrameCount_ / (statsAccumMs_ / 1000.0);
            statsFrameTimeMs_ = statsAccumMs_ / statsFrameCount_;
            double n = statsFrameCount_;
            phaseJsMs_ = accumJsMs_ / n;
            phaseLayoutMs_ = accumLayoutMs_ / n;
            phaseRasterMs_ = accumRasterMs_ / n;
            phaseGpuMs_ = accumGpuMs_ / n;
            phaseGlStateMs_ = accumGlStateMs_ / n;
            phaseDrawMs_ = accumDrawMs_ / n;
            phaseUploadMs_ = accumUploadMs_ / n;
            accumJsMs_ = accumLayoutMs_ = accumRasterMs_ = accumGpuMs_ = accumGlStateMs_ = 0.0;
            accumDrawMs_ = accumUploadMs_ = 0.0;
            statsAccumMs_ = 0.0;
            statsFrameCount_ = 0;
            statsMinFrameMs_ = 999.0;
            statsMaxFrameMs_ = 0.0;
            uiDirty_ = true;  // refresh overlay

            // Push perf data to system overlay JS
            {
                updateSystemPerf(statsFps_, statsFrameTimeMs_,
                                           phaseJsMs_, phaseLayoutMs_,
                                           phaseRasterMs_, phaseGpuMs_,
                                           phaseDrawMs_,
                                           viewportWidth_, viewportHeight_);
            }
        }
    }

    // --- Physics thread shutdown ---
    if (physicsWorld_) {
        physicsWorld_->shutdown();
    }

    // --- Layout thread shutdown ---
    layoutShared_.state.store(kLayoutShutdown, std::memory_order_release);
    layoutShared_.state.notify_one();
    if (layoutThread_.joinable()) {
        layoutThread_.join();
    }

    // --- Raster thread shutdown ---
    rasterShared_.state.store(kRasterShutdown, std::memory_order_release);
    rasterShared_.state.notify_one();
    if (rasterThread_.joinable()) {
        rasterThread_.join();
    }
    if (rasterGLContext_) {
        SDL_GL_DestroyContext(rasterGLContext_);
        rasterGLContext_ = nullptr;
    }

    // Stop canvas threads before GL context cleanup
    for (auto& cs : canvasScenes_) {
        if (cs) cs->stopThread();
    }

    SDL_RemoveEventWatch(modalEventWatcher, this);
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

void Engine::handleResize(int w, int h) {
    viewportWidth_ = w;
    viewportHeight_ = h;
    uiDirty_ = true;
    hasRenderedOnce_ = false;
    drawTraversal_->setViewport(w, contentHeight(), contentTop());
    // WebGL canvases resize based on element layout, not viewport — handled per-frame
    {
        resizeSystemPanels(w, h);
    }
    int ch = contentHeight();
    if (document_) {
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_);
        document_->resolveStyles();
        document_->performLayout(static_cast<float>(w), static_cast<float>(ch), *textMetrics_);
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

        // Update innerWidth / innerHeight (innerHeight excludes the menu inset
        // so apps see a web-like viewport that matches their layout area).
        JS_SetPropertyStr(ctx, global, "innerWidth", JS_NewInt32(ctx, w));
        JS_SetPropertyStr(ctx, global, "innerHeight", JS_NewInt32(ctx, ch));

        // Update canvas element width/height attributes via JS
        JSValue fn = JS_Eval(ctx, js_canvas_resize, strlen(js_canvas_resize),
                             "<resize>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsFunction(ctx, fn)) {
            JSValue args[2] = { JS_NewInt32(ctx, w), JS_NewInt32(ctx, ch) };
            JSValue ret = JS_Call(ctx, fn, global, 2, args);
            JS_FreeValue(ctx, ret);
            JS_FreeValue(ctx, args[0]);
            JS_FreeValue(ctx, args[1]);
        }
        JS_FreeValue(ctx, fn);

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

// Input handling methods (handleMouse*, handleKey*, handleTextInput,
// handleWheel, advanceFocus, dispatchInputEvent) are in input_handling.cpp.


dom::Element* Engine::hitTest(float x, float y) {
    // x, y are already in document space (scroll-adjusted by callers)
    if (!document_ || !document_->documentElement())
        return document_ ? document_->body() : nullptr;

    auto* root = document_->layoutRoot();
    if (!root) return document_->body();
    auto* node = htmlayout::layout::hitTest(root, x, y);
    auto* hit = layout::LayoutNodeAdapter::elementFor(node);
    // The <html> element fills the viewport — stray clicks outside any
    // laid-out content should still resolve to the document element.
    if (!hit) return document_->documentElement();
    return hit;
}

// ---------------------------------------------------------------------------
// Event dispatch to JS (delegates to shared implementation)
// ---------------------------------------------------------------------------

void Engine::dispatchEvent(dom::Element* target, dom::Event& event) {
    if (!target || !jsRuntime_) return;
    js::dispatchDomEvent(jsRuntime_->getContext(), target, event);
}

// Walk the document + shadow trees and pump any pending HTMLMediaElement
// events (loadedmetadata, timeupdate, ended) on each ElVideo. Called from
// the main thread because QuickJS is not thread-safe; ElVideo::draw() runs
// on the raster thread and deliberately does not touch JS.
static void pumpVideoEventsWalk(dom::Element* el, bool& anyPlaying) {
    if (!el) return;
    if (auto* v = el->videoControl()) {
        v->pumpEvents();
        if (v->isPlaying()) anyPlaying = true;
    }
    el->forEachComposedChild([&](dom::Element* c) {
        pumpVideoEventsWalk(c, anyPlaying);
    });
}
void Engine::pumpVideoEvents() {
    // The engine's initial layout flush runs BEFORE user script, so
    // dispatching loadedmetadata / timeupdate there would drop events on
    // the floor (no listeners registered yet). Wait until the user code
    // has had a chance to run — the windowed main loop and any JS-driven
    // flush() set this flag before pumping.
    if (!mediaEventsArmed_) return;
    if (!document_) return;
    bool anyPlaying = false;
    pumpVideoEventsWalk(document_->documentElement(), anyPlaying);
    // Playing <video> elements don't mutate the DOM, so nothing else would
    // mark the document dirty. Force a re-raster each frame while any video
    // is advancing so ElVideo::draw() keeps calling pipeline_->advance() and
    // presenting new frames.
    if (anyPlaying) {
        document_->markDirty();
        // markDirty alone isn't enough in the windowed main loop: if the
        // layout thread is already idle, nothing sets uiDirty_ so the
        // raster signal path is skipped. Set uiDirty_ directly so the
        // "no layout this frame" branch at engine.cpp still signals raster.
        uiDirty_ = true;
    }
}

void Engine::drawSelectionHighlight(render::Renderer* renderer, float docOffsetY) {
    if (!renderer || !document_ || !textMetrics_) return;
    auto* sel = document_->selection();
    if (!sel || sel->isCollapsed() || sel->rangeCount() == 0) return;
    const auto* range = sel->getRangeAt(0);
    if (!range || !range->startContainer() || !range->endContainer()) return;

    auto rects = layout::getSelectionRects(document_.get(),
                                           range->startContainer(),
                                           range->startOffset(),
                                           range->endContainer(),
                                           range->endOffset(),
                                           *textMetrics_);
    // Accent with transparency — keeps underlying glyphs legible. Blue-ish
    // default; apps can theme later if needed.
    render::Color hl{0x33, 0x77, 0xff, 0x55};
    for (const auto& r : rects) {
        renderer->fillRect(r.x, r.y + docOffsetY, r.width, r.height, hl);
    }
}

void Engine::drawElementScrollbars(render::Renderer* renderer,
                                   dom::Element* root,
                                   float offsetX, float offsetY) {
    if (!renderer || !root) return;
    std::function<void(dom::Element*, float, float)> walk;
    walk = [&](dom::Element* elem, float ox, float oy) {
        if (!elem) return;
        auto& style = elem->computedStyle();
        auto dispIt = style.find("display");
        if (dispIt != style.end() && dispIt->second == "none") return;

        auto& lbox = elem->layoutBox();
        float absX = lbox.contentRect.x + ox;
        float absY = lbox.contentRect.y + oy;

        std::string ov = getOverflowY(style);
        if (overflowScrollable(ov)) {
            float maxST = maxScrollTop(elem);
            if (maxST > 0) {
                float viewH = lbox.contentRect.height;
                float contentH = viewH + maxST;
                float bx = absX - lbox.padding.left - lbox.border.left;
                float by = absY - lbox.padding.top - lbox.border.top;
                float bw = lbox.fullWidth();
                float bh = lbox.fullHeight();

                auto& es = elementScrollbar_.style();
                auto m = elementScrollbar_.layout(
                    bx + bw - es.width - es.margin,
                    by, bh, contentH, viewH,
                    elem->scrollTopValue());
                elementScrollbar_.draw(renderer, m);
            }
        }

        float childOx = absX;
        float childOy = absY - elem->scrollTopValue();
        elem->forEachComposedChild([&](dom::Element* child) {
            walk(child, childOx, childOy);
        });
    };
    walk(root, offsetX, offsetY);
}


// ---------------------------------------------------------------------------
// Replaced element control initialization
// ---------------------------------------------------------------------------

void Engine::ensureReplacedElements(dom::Element* elem) {
    JSContext* jsCtx = jsRuntime_ ? jsRuntime_->getContext() : nullptr;
    bro::engine::ensureReplacedElements(elem, renderer_.get(), jsCtx,
                                         audioEngine_.get());
}

// Headless/capture API (flush, advanceTime, eval, screenshot, capturePixels,
// querySelector, dispatchClickOn) is in headless_api.cpp.
} // namespace bro::engine
