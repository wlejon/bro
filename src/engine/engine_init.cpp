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
#include "js/gamepad_bindings.h"
#include "js/custom_elements.h"
#include "js/webgl2_bindings.h"
#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
#include "js/video_bindings.h"
#include "js/worker.h"
#if BRO_WITH_PHYSICS
#include "js/physics_bindings.h"
#endif
#include "js/scene_bindings.h"
#include "js/menu_bindings.h"
#include "js/time_bindings.h"
#include "js/gizmo_bindings.h"
#include "js/mesh_bindings.h"
#include "js/flora_bindings.h"
#include "js/math_bindings.h"
#include "js/rigging_bindings.h"
#include "js/ai_bindings.h"
#include "js/gpu_bindings.h"
#include "js/diffusion_bindings.h"
#include "js/lm_bindings.h"
#include "js/stt_bindings.h"
#include "js/tts_bindings.h"
#include "js/diar_bindings.h"
#include "js/rave_bindings.h"
#include "js/triposplat_bindings.h"
#include "js/motion_bindings.h"
#include "js/vision_bindings.h"
#include "js/wake_bindings.h"
#include "js/gesture_bindings.h"
#include "js/kws_bindings.h"
#include "js/listen_bindings.h"
#if BRO_WITH_SOUNDML
#include "js/listen_host.h"  // fat header (pulls brosoundml/brotensor)
#endif
#include "js/sense_bindings.h"
#include "js/mic_bindings.h"
#include "js/terrain_bindings.h"
#include "js/tile_bindings.h"
#include "js/net_bindings.h"
#include "js/steam_bindings.h"
#include "js/server_bindings.h"
#include "js/headless_bindings.h"

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "audio_inference/audio_inference.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#include "steam/steam_service.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
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
    appDir_ = config.appDir;
    titleOverride_ = config.title;

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
        // Shared ES-module std lib. App-local `<app>/std` wins (e.g. a tool
        // piloting the lib in-tree); otherwise the project-root `std/`. Apps
        // import it by virtual path (`/std/dom.js`) regardless of where it
        // lives, so moving the folder needs no import edits.
        tryMount("std", "std");
        // `/app` always points at the running app's own root, so an app's
        // internal modules import each other by absolute virtual path
        // (`/app/lib/foo.js`) without fragile `../` traversal.
        if (!config.appDir.empty()) {
            std::error_code ec;
            assetMounts_.addMount("/app", fs::absolute(config.appDir, ec).string());
        }
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
    frameCapIntervalMs_ = gfx.maxFps > 0.0 ? 1000.0 / gfx.maxFps : 0.0;
    inputConfig_.scrollSpeed = inp.scrollSpeed;
    inputConfig_.doubleClickThresholdMs = inp.doubleClickThresholdMs;
    inputConfig_.doubleClickDistancePx = inp.doubleClickDistancePx;
    inputConfig_.overlayToggleKey = inp.overlayToggleKey;

    // === Common JS runtime initialization ===

    // 4. JS runtime + engine-level services. Per-context binding INSTALLS live
    //    in installCoreBindings()/initAppRealm() so a top-level
    //    location.reload() can re-run them against a fresh realm; only the
    //    engine-owned objects (services, worlds, pumps) are created here, once.
    jsRuntime_ = std::make_unique<js::Runtime>();
    jsRuntime_->setModuleLoader(&assetMounts_);

    // Wire brokit logging through bro's LOG_* macros
    brokit::Runtime::setLogCallback([](brokit::Runtime::LogLevel level, const std::string& msg) {
        switch (level) {
            case brokit::Runtime::LogLevel::Warn:  LOG_WARN("[console] %s", msg.c_str()); break;
            case brokit::Runtime::LogLevel::Error: LOG_ERROR("[console] %s", msg.c_str()); break;
            default: LOG_INFO("[console] %s", msg.c_str()); break;
        }
    });

    timers_ = std::make_unique<js::Timers>();

    // Seed the timer time base so setTimeout/setInterval use the correct clock.
    // In headless mode this is virtual time; in windowed/server mode, real time.
    // The bro.time scaled clock is seeded from the same value — timers only
    // ever tick with engineNowMs_ from here on, so deadlines and the scaled
    // clock can never diverge.
    engineNowMs_ = displayMode_ == DisplayMode::Headless ? virtualTime_
                                                         : util::currentTimeMs();
    timers_->tick(engineNowMs_);

    // Physics world (all modes). With BRO_WITH_PHYSICS off there is no physics
    // world and the `Physics` JS class is simply absent (advanced apps
    // feature-detect `typeof Physics`).
#if BRO_WITH_PHYSICS
    physicsWorld_ = std::make_unique<physics::PhysicsWorld>();
    physicsWorld_->init();
    if (displayMode_ != DisplayMode::Headless)
        physicsWorld_->startThread();
#endif

    // Network service (all modes). NetService owns GNS on its own thread;
    // bindings hold a per-context subscriber that polls each frame, delivered
    // via a frame pump so no `if (netService_)` sits in the hot loop. With
    // BRO_WITH_NET off, the stub install() publishes an unavailable bro.net
    // namespace and no pump is registered.
#if BRO_WITH_NET
    netService_ = std::make_unique<net::NetService>();
    framePumps_.push_back([this] {
        js::NetBindings::poll(jsRuntime_->getContext());
        jsRuntime_->executePendingJobs();
    });
#endif

    // Steam service (all modes). Always-present probe like bro.gpu: in a stub
    // build (BRO_WITH_STEAM=OFF) the service is inert and bro.steam.available
    // is false. When enabled, SteamService owns the Steamworks API on its own
    // thread; bindings hold a per-context subscriber that polls each frame.
    steamService_ = std::make_unique<steam::SteamService>();

    serverStartTime_ = util::currentTimeMs();

    // === Server mode: lightweight init — no rendering, DOM, or audio ===

    if (displayMode_ == DisplayMode::Server) {
        // Mode-independent bindings (brokit, timers, physics, ML tower, net/
        // steam/server). Servers never reload, so this is their only install.
        installCoreBindings(jsRuntime_->getContext());

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
#if BRO_WITH_NET
        js::installWorkerBindings(jsRuntime_->getContext(), config.appDir,
                                  netService_.get(), &assetMounts_);
#else
        js::installWorkerBindings(jsRuntime_->getContext(), config.appDir,
                                  nullptr, &assetMounts_);
#endif

        LOG_INFO("Server mode initialized (no rendering, no DOM, no audio)");
        return;
    }

    // === Windowed / Headless initialization (rendering + DOM) ===

    // hasGL: true when we have a GPU context (windowed, or headless with GPU)
    const bool hasGL = (displayMode_ == DisplayMode::Windowed) || config.graphics.useGPU;

    if (hasGL) {
        // Create window (hidden for headless, visible for windowed)
        bool hidden = (displayMode_ == DisplayMode::Headless);
        try {
            window_ = std::make_unique<platform::Window>("Bro",
                static_cast<uint32_t>(gfx.width),
                static_cast<uint32_t>(gfx.height), hidden,
                gfx.resizable, gfx.vsync, config.graphics.borderless);

            // Startup window management from bro.json. These are startup-only
            // config (GraphicsConfig), not persisted settings — runtime
            // control lives in bro.window.*. Flags and resize limits apply to
            // the hidden headless window too: they're pure window state, so
            // bro.window's getters round-trip in tests. Positioning is
            // skipped when hidden — it depends on the desktop the suite
            // happens to run on.
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
                // Explicit position wins over display centering.
                if (wcfg.windowX != kWindowPosUnset && wcfg.windowY != kWindowPosUnset)
                    window_->setPosition(wcfg.windowX, wcfg.windowY);
            }

            // Taskbar / Alt-Tab icon. Shipped with system/ alongside the binary
            // (scripts/package-release.sh copies the whole system/ tree). Skip in
            // headless where the window is hidden anyway.
            if (!hidden) {
                window_->setIcon("system/icon.png");
                // Real OS display scale → window.devicePixelRatio. Headless
                // deliberately keeps the 1.0 default: the hidden window sits on
                // a real display and would report its scale, making test output
                // depend on the desktop the suite happens to run on.
                displayScale_ = window_->getDisplayScale();
            }

            // GL context (shader programs + helpers)
            gl_ = std::make_unique<render::GLContext>(*window_);

            // Renderer (Skia raster + OpenGL display)
            renderer_ = render::createRenderer(gl_.get());
            if (!renderer_) {
                throw std::runtime_error("Failed to create renderer");
            }
        } catch (const std::exception& e) {
            // A headless run on a box with no usable OpenGL (no driver, only
            // software GL 1.1) can't get a GPU context. Rather than abort, fall
            // back to the CPU raster path — the identical state --no-gpu produces
            // (no window_/gl_, RasterRenderer; the rest of the engine keys off
            // gl_ being null). Windowed mode has no meaningful fallback, so let
            // it propagate.
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
        // No GPU: CPU-only Skia renderer, no window/GL
        renderer_ = std::make_unique<render::RasterRenderer>();
    }

    if (displayMode_ == DisplayMode::Headless) {
        virtualTime_ = util::currentTimeMs();
        // Keep the bro.time scaled clock in lockstep with virtual time (they
        // advance together in advanceTime; at scale 1 they stay identical).
        engineNowMs_ = virtualTime_;
    }

    // 4b. Audio engine (bindings install in initAppRealm)
    audioEngine_ = std::make_unique<broaudio::Engine>();
    if (displayMode_ == DisplayMode::Windowed || config.realAudio) {
        audioEngine_->init();
    } else {
        audioEngine_->initHeadless();
    }

    // Audio-inference subsystem: owns the background worker thread that runs
    // audio-driven NN models (wake word today) off the audio thread and off the
    // main thread. Threaded in Windowed/Server; pumped inline in Headless for
    // deterministic tests — the same parity pattern as physicsWorld_ above.
    audioInference_ = std::make_unique<AudioInference>();
    if (displayMode_ != DisplayMode::Headless)
        audioInference_->startThread();

    // The shared listen host: ONE raw (no-AGC) mic tap + ring + inference
    // task driving a brosoundml::ListenBus that bro.kws's spotter and
    // bro.sense's hub join as members — one PCEN feature pass, one PhonemeNet
    // forward, N listeners. (bro.wake stays on its own AGC'd tap until
    // retrained AGC-free.) Engine-level (no JSContext), so it survives a
    // location.reload() realm swap. Inert until a member attaches.
#if BRO_WITH_SOUNDML
    js::installListenHost(audioEngine_.get(), audioInference_.get());

    // Deliver wake/kws/gesture fires the self-paced audio-inference worker (or,
    // headless, the inline pump) published since last frame. Registered only
    // when the soundml models are built, so the frame loop carries no stubbed
    // hook when they aren't.
    framePumps_.push_back([this] {
        JSContext* c = jsRuntime_->getContext();
        js::tickWake(c);
        js::tickKws(c);
        js::tickGesture(c);
    });
#endif

    // bro.mic chunk delivery — audio tier, independent of the AI models above.
    framePumps_.push_back([this] { js::tickMic(jsRuntime_->getContext()); });

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
            if (key == "maxFps" || key == "*")
                frameCapIntervalMs_ = gfx.maxFps > 0.0 ? 1000.0 / gfx.maxFps : 0.0;
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
        if (category == "appearance" || category == "*") {
            // Re-evaluate @media (prefers-color-scheme) everywhere; documents
            // whose effective scheme changed mark themselves dirty.
            applyColorScheme();
        }
    });

    // Default menu tree (File/Edit/View). App items are re-added by app JS on
    // every realm run; bro.menu bindings install in initAppRealm.
    resetMenuBarDefaults();

    // Engine gizmo (bro.gizmo.*) — translate/rotate/scale handles with
    // mouse-driven picking and drag interaction. 3D-only. Bindings install in
    // initAppRealm.
#if BRO_WITH_3D
    gizmo_ = std::make_unique<GizmoManager>();
#endif

    // 5. Layout helpers. drawTraversal_ writes through a RecordingRenderer
    //    (wraps renderer_ for measurement + font handle issuance) so the paint
    //    walk emits commands into a CommandBuffer instead of issuing Skia
    //    work — the raster thread replays the buffer without touching the DOM.
    //    textMetrics_ stays bound to renderer_ since it only does measurement.
    recordingRenderer_ = std::make_unique<render::RecordingRenderer>(nullptr, renderer_.get());
    drawTraversal_ = std::make_unique<layout::DrawTraversal>(recordingRenderer_.get());
    textMetrics_ = std::make_unique<layout::SkiaTextMetrics>(renderer_.get());

    // 5b. Event loop (windowed). Created before the app realm so window.close()
    //     binds during initAppRealm — and rebinds on a location.reload() realm.
    if (displayMode_ == DisplayMode::Windowed) {
        eventLoop_ = std::make_unique<platform::EventLoop>();
    }

    // 6-10. The app realm: every per-context binding install plus the app
    //       document itself (manifest → parse → scripts → fonts → layout →
    //       iframes → DOMContentLoaded/load). Top-level location.reload()
    //       re-runs exactly this on a fresh context (see performAppReload).
    initAppRealm();

    // === Mode-specific post-init ===

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

    // Headless: flush an initial frame. Style + layout already ran above
    // (inside initAppRealm, before the load dispatch); flush() re-layouts only
    // if a load handler dirtied the document, then rasterizes.
    if (displayMode_ == DisplayMode::Headless) {
        flush();
    }
}

// Build (or rebuild) everything bound to the primary JSContext or the app
// document, on the CURRENT primary context. Called once from the constructor
// and again — on a fresh context — by performAppReload() (top-level
// location.reload()). Every engine-level object it references (renderer,
// window, audio, services, workers, settings, gizmo, event loop) must already
// exist. Windowed + Headless only; Server mode has its own lightweight path
// in the constructor.
void Engine::initAppRealm() {
    JSContext* appCtx = jsRuntime_->getContext();

    // Fresh realm ⇒ the document lifecycle starts over.
    documentReadyState_ = "loading";

    // Mode-independent core (brokit, timers, physics, mesh/flora/math/ai,
    // the ML tower, terrain/tile, net/steam/server).
    installCoreBindings(appCtx);

    // Audio + the audio-ML surfaces (wake/listen/kws/sense/gesture/mic). The
    // engines and the shared listen host are engine-level and already running;
    // these bind the JS surfaces of the new realm to them.
    js::AudioBindings::install(appCtx, audioEngine_.get());
    js::installWakeBindings(appCtx, audioEngine_.get(), audioInference_.get());
    js::installListenBindings(appCtx);
    js::installKwsBindings(appCtx, audioEngine_.get(), audioInference_.get());
    js::installSenseBindings(appCtx, audioEngine_.get(), audioInference_.get());
    js::installGestureBindings(appCtx, audioEngine_.get(), audioInference_.get());
    js::installMicBindings(appCtx, audioEngine_.get());

    // Scene graph bindings (3D-only; needs renderer)
#if BRO_WITH_3D
    js::SceneBindings::install(appCtx);
#endif

    // Global pause + timescale (bro.time.*) — scale/paused/now over the
    // engine's scaled clock.
    js::TimeBindings::install(appCtx, this);

    // Menu bar bindings (bro.menu.*). The default tree is engine-level
    // (resetMenuBarDefaults in the constructor / performAppReload).
    js::MenuBindings::install(appCtx, this);

    // Engine gizmo (bro.gizmo.*) — 3D-only.
#if BRO_WITH_3D
    js::GizmoBindings::install(appCtx, this);
#endif

    // 6. Load the application
    manifest_ = AppLoader::loadApp(appDir_, &assetMounts_);
    std::string html = AppLoader::loadFile(manifest_.htmlPath);
    if (html.empty()) {
        throw std::runtime_error("Failed to load index.html from " + appDir_);
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
    // App document draws in content space (viewportTop 0); the compositor
    // places its layers at (0, contentTop()).
    drawTraversal_->setViewport(contentWidth(), contentHeight(), 0);

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
    // @media queries evaluate against the content-space viewport; set it
    // before parse() so stylesheets are filtered on first add. Same for the
    // color scheme (settings override or OS theme).
    document_->setMediaViewport(static_cast<float>(contentWidth()),
                                static_cast<float>(contentHeight()));
    document_->setMediaColorScheme(effectiveColorScheme());
    // @import resolution: read the referenced CSS relative to the app root
    // (same path rules as every other app asset).
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

    // 8a. Inject extracted templates back into the DOM tree
    if (!templateBlocks.empty())
        document_->injectTemplates(templateBlocks);

    // 8b. Set window title (windowed only)
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

    // 9. Set up window/navigator/location/history BEFORE DOM bindings
    js::installWindowBindings(jsRuntime_->getContext(), viewportWidth_, contentHeight(),
                              displayScale_);

    // 9a1. bro.window.* — runtime window management (state, borderless,
    //      always-on-top, size limits, position, displays). App realm only.
    js::installBroWindowBindings(jsRuntime_->getContext(), window_.get(),
                                 displayMode_ == DisplayMode::Headless);

    // 9a0. location.reload() — the polyfill's method calls this hook when
    //      present. Queues a full app-realm reload; deferred to a safe point
    //      (see requestAppReload). Iframe sub-documents get their own hook in
    //      createIframeDoc; system panels get none (reload stays a no-op).
    {
        JSValue global = JS_GetGlobalObject(appCtx);
        JSValue ptrVal = JS_NewInt64(appCtx, static_cast<int64_t>(
                                                 reinterpret_cast<intptr_t>(this)));
        JS_SetPropertyStr(appCtx, global, "__bro_location_reload",
            JS_NewCFunctionData(appCtx, [](JSContext* cx, JSValue, int, JSValue*,
                                           int, JSValue* fdata) -> JSValue {
                int64_t p = 0;
                JS_ToInt64(cx, &p, fdata[0]);
                auto* self = reinterpret_cast<Engine*>(static_cast<intptr_t>(p));
                if (self) self->requestAppReload();
                return JS_UNDEFINED;
            }, 0, 0, 1, &ptrVal));
        JS_FreeValue(appCtx, ptrVal);
        JS_FreeValue(appCtx, global);
    }

    // 9y. navigator.getGamepads() + gamepadconnected/disconnected plumbing.
    //     Works in windowed (SDL gamepad events) and headless (virtual pads).
    js::installGamepadBindings(jsRuntime_->getContext(), this);

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
    js::ImageBitmapBindings::install(jsRuntime_->getContext());
    js::VideoBindings::install(jsRuntime_->getContext(), manifest_.basePath);
#if BRO_WITH_3D
    js::SceneBindings::setAppContext(manifest_.basePath, &assetMounts_);
    js::TileBindings::setAppContext(manifest_.basePath, &assetMounts_);
#endif
#if BRO_WITH_DIFFUSION
    js::setDiffusionAppContext(manifest_.basePath, &assetMounts_);
#endif
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
                        // Seed the bitmap size from width/height attributes that
                        // may have been assigned before getContext() ran. Without
                        // this, an offscreen canvas (never in the DOM, so no
                        // layout box to fall back on) silently defaults to
                        // 300x150 and any larger drawing is cropped + rescaled.
                        const std::string wAttr = el->getAttribute("width");
                        const std::string hAttr = el->getAttribute("height");
                        if (!wAttr.empty()) scene->setIntrinsicWidth(std::atoi(wAttr.c_str()));
                        if (!hAttr.empty()) scene->setIntrinsicHeight(std::atoi(hAttr.c_str()));
                        scene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                            auto* elem = static_cast<dom::Element*>(ud);
                            if (!elem->parentNode()) {
                                ox = oy = ow = oh = 0;
                                return;
                            }
                            dom::AbsoluteRect r = dom::absoluteContentBox(elem);
                            ox = r.x; oy = r.y; ow = r.width; oh = r.height;
                        }, el);
                        scene->setDetachedCallback([](void* ud) -> bool {
                            // Walk up to check if connected to the document
                            auto* n = static_cast<dom::Element*>(ud);
                            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                            return n->tagName() != "html" && n->tagName() != "HTML";
                        }, el);
                        scene->setLiveCheck([](void* doc, void* node) -> bool {
                            return static_cast<dom::Document*>(doc)->isNodeLive(
                                static_cast<dom::Element*>(node));
                        }, el->document());
                    }
                    auto* ptr = scene.get();
                    if (el) el->setCanvasScene(ptr, &canvas::CanvasScene::onBackingElementDestroyed);
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
#if BRO_WITH_3D
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
                            dom::AbsoluteRect r = dom::absoluteContentBox(elem);
                            ox = r.x; oy = r.y; ow = r.width; oh = r.height;
                        }, el);
                        canvasScene->setDetachedCallback([](void* ud) -> bool {
                            auto* n = static_cast<dom::Element*>(ud);
                            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                            return n->tagName() != "html" && n->tagName() != "HTML";
                        }, el);
                        canvasScene->setLiveCheck([](void* doc, void* node) -> bool {
                            return static_cast<dom::Document*>(doc)->isNodeLive(
                                static_cast<dom::Element*>(node));
                        }, el->document());
                    }
                    auto* csPtr = canvasScene.get();
                    if (el) el->setCanvasScene(csPtr, &canvas::CanvasScene::onBackingElementDestroyed);
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
#endif  // BRO_WITH_3D
                return JS_NULL;
            });
    } else {
        // CPU path: 2D canvas + scene graph, no WebGL
        js::DomBindings::setGetContextFactory(jsRuntime_->getContext(),
            [this](JSContext* ctx, dom::Element* el, const std::string& type) -> JSValue {
                auto canvasScene = std::make_unique<canvas::CanvasScene>(renderer_.get());
                if (el) {
                    // Honour width/height attributes set before getContext() —
                    // see the matching comment in the GPU 2d factory above.
                    const std::string wAttr = el->getAttribute("width");
                    const std::string hAttr = el->getAttribute("height");
                    if (!wAttr.empty()) canvasScene->setIntrinsicWidth(std::atoi(wAttr.c_str()));
                    if (!hAttr.empty()) canvasScene->setIntrinsicHeight(std::atoi(hAttr.c_str()));
                    canvasScene->setLayoutCallback([](void* ud, float& ox, float& oy, float& ow, float& oh) {
                        auto* elem = static_cast<dom::Element*>(ud);
                        if (!elem->parentNode()) {
                            ox = oy = ow = oh = 0;
                            return;
                        }
                        dom::AbsoluteRect r = dom::absoluteContentBox(elem);
                        ox = r.x; oy = r.y; ow = r.width; oh = r.height;
                    }, el);
                    canvasScene->setDetachedCallback([](void* ud) -> bool {
                        auto* n = static_cast<dom::Element*>(ud);
                        while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
                        return n->tagName() != "html" && n->tagName() != "HTML";
                    }, el);
                    canvasScene->setLiveCheck([](void* doc, void* node) -> bool {
                        return static_cast<dom::Document*>(doc)->isNodeLive(
                            static_cast<dom::Element*>(node));
                    }, el->document());
                }
                auto* csPtr = canvasScene.get();
                if (el) el->setCanvasScene(csPtr, &canvas::CanvasScene::onBackingElementDestroyed);
                canvasScene->init(nullptr);
                canvasSceneRegistry_[canvasScene->sceneId()] = csPtr;
                canvasScenes_.push_back(std::move(canvasScene));
#if BRO_WITH_3D
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
#endif  // BRO_WITH_3D
                return js::CanvasBindings::wrapContext2D(ctx, csPtr);
            });
    }

    // 9d. Install Worker bindings
#if BRO_WITH_NET
    js::installWorkerBindings(jsRuntime_->getContext(), manifest_.basePath,
                              netService_.get(), &assetMounts_);
#else
    js::installWorkerBindings(jsRuntime_->getContext(), manifest_.basePath,
                              nullptr, &assetMounts_);
#endif

    // 9e. window.close() (windowed — the event loop was created before this
    //     realm) and the headless script globals (screenshot/advanceTime/...;
    //     the headless driver also installs these on first boot, which is
    //     harmless — same engine pointer, same functions. Re-installing here
    //     is what keeps them alive across a location.reload() realm swap).
    if (eventLoop_)
        js::installWindowClose(appCtx, eventLoop_.get());
    if (displayMode_ == DisplayMode::Headless)
        js::installHeadlessBindings(appCtx, this);

    // 10. Load and execute scripts (external + inline, in document order).
    //     `type="module"` scripts go through evalModule so `import`/`export`
    //     and the file-based module loader (mount-aware for `/lib/...`) work.
    //     Inline modules resolve relative imports against the app base path.
    for (auto& script : manifest_.scripts) {
        if (script.isInline()) {
            if (script.isModule) {
                std::string filename = manifest_.basePath + "/<inline-module>";
                if (!jsRuntime_->evalModule(script.code, filename)) {
                    LOG_ERROR("Failed to execute inline module script");
                }
            } else if (!jsRuntime_->eval(script.code, "<inline>")) {
                LOG_ERROR("Failed to execute inline script");
            }
        } else {
            std::string code = AppLoader::loadFile(script.path);
            if (!code.empty()) {
                bool ok = script.isModule
                              ? jsRuntime_->evalModule(code, script.path)
                              : jsRuntime_->eval(code, script.path);
                if (!ok) {
                    LOG_ERROR("Failed to execute script: %s", script.path.c_str());
                }
            }
        }
    }

    // 10a. Load @font-face custom fonts, then run an initial style + layout
    //      pass. This MUST happen before DOMContentLoaded/load dispatch:
    //      browsers complete layout before those events, and apps measure the
    //      DOM (clientWidth, getBoundingClientRect, ...) in load handlers — the
    //      universal, correct idiom. Without a layout tree those reads return
    //      pre-layout zeros. Layout needs font metrics, so loadCustomFonts()
    //      runs first.
    loadCustomFonts();
    if (document_) {
        ensureReplacedElements(document_->documentElement());
        layout::ElementRefAdapter::setHoveredElement(hoveredElement_.get());
        // Transitions live on the bro.time scaled clock — registration and
        // every later tick (layout-thread snapshot / headless flush) must use
        // the same clock, or a load-time transition's startTime could sit
        // ahead of the first tick and never progress.
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
        // Instantiate sub-documents for any <iframe src> now that they're laid
        // out (createIframeDoc reads each iframe's content box).
        syncIframes();
    }

    // 10b. Dispatch DOMContentLoaded on document
    {
        JSContext* ctx = jsRuntime_->getContext();
        JSValue global = JS_GetGlobalObject(ctx);
        // Parsing + scripts + initial layout are done — the document is now
        // "interactive". Set this before dispatching DOMContentLoaded so
        // readyState reads correctly inside the handler.
        documentReadyState_ = "interactive";
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

            // load — the document is fully "complete" before this fires.
            documentReadyState_ = "complete";
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

    // User script has not run yet during construction (and a reload's realm
    // has just finished its scripts). Arm media events so the next pump
    // (first JS-driven flush / first main-loop tick) fires queued
    // loadedmetadata / timeupdate.
    mediaEventsArmed_ = true;
}

// Mode-independent per-context installs — everything a bro realm gets no
// matter the display mode: brokit's web/system APIs, timers, physics, the
// mesh/flora/math/rigging/game-AI families, the ML tower, terrain/tile, and
// the net/steam/server surfaces. The engine-level services these bind to
// (physicsWorld_, netService_, steamService_) are created once in the
// constructor; this only wires a (possibly fresh) context to them.
void Engine::installCoreBindings(JSContext* ctx) {
    // Install all brokit APIs (console, timers, URL, crypto, encoding, fetch, etc.)
    brokit::api::installAll(ctx);

    js::Timers::install(ctx, timers_.get());

#if BRO_WITH_PHYSICS
    js::PhysicsBindings::install(ctx, physicsWorld_.get());
#endif

    // Mesh + rigging bindings (Mesh class wrapping bromesh — 3D-only).
#if BRO_WITH_3D
    js::MeshBindings::install(ctx);
#endif

    // Flora bindings (broflora ecosystem sim — bro.flora.* — all modes)
    js::FloraBindings::install(ctx);

    // Math bindings (bro.math.* — SpatialHash3D and future bromath types)
    js::MathBindings::install(ctx);

#if BRO_WITH_3D
    js::RiggingBindings::install(ctx);
#endif

    // AI bindings (game agent: navgrid, pathfinding, steering). When BRO_WITH_GAMEAI
    // is off, install() is the feature-stub that installs an unavailable bro.ai.
    js::AIBindings::install(ctx);

    // bro.gpu (runtime backend probe via brotensor). Always present — reports
    // whether the ML loaders below will default to a GPU or fall back to CPU.
    js::installGpuBindings(ctx);

    // bro.tensor / bro.diffusion / bro.lm / bro.stt / bro.tts / bro.diar /
    // bro.rave / bro.vision / bro.triposplat / bro.motion — the ML tower.
    // Real bindings or `{ available: false }` stubs per the build profile.
    js::installTensorBindings(ctx);
    js::installDiffusionBindings(ctx);
    js::installLmBindings(ctx);
    js::installSttBindings(ctx);
    js::installTtsBindings(ctx);
    js::installDiarBindings(ctx);
    js::installRaveBindings(ctx);
    js::installVisionBindings(ctx);
    js::installTriposplatBindings(ctx);
    js::installMotionBindings(ctx);

    // Terrain + tile-world bindings (voxel terrain / chunked tile grid) — 3D-only.
#if BRO_WITH_3D
    js::TerrainBindings::install(ctx);
    js::TileBindings::install(ctx);
#endif

    // bro.net — per-context subscriber onto the engine-level NetService (the
    // frame pump was registered once, in the constructor). Stub without NET.
#if BRO_WITH_NET
    js::NetBindings::install(ctx, netService_.get());
#else
    js::NetBindings::install(ctx, nullptr);
#endif

    // bro.steam — always-present probe; per-context subscriber when enabled.
    js::SteamBindings::install(ctx, steamService_.get());

    // bro.server.* (all modes) — in windowed mode this lets the process host
    // an in-process server script (e.g. the launcher running apps/fps/server.js).
    js::ServerBindings::install(ctx, this);
}

// (Re)build the engine's default menu tree. On a location.reload() the app
// realm that added custom items is gone (its handlers were released), so the
// bar drops back to the defaults and the fresh realm re-adds its own.
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
        // A location.reload() re-runs this against the same @font-face rules;
        // the faces are already registered on every renderer (main, layout,
        // raster), so skip them rather than re-reading and re-registering —
        // loadedFonts_ would otherwise grow by the full set per reload.
        bool alreadyLoaded = std::any_of(
            loadedFonts_.begin(), loadedFonts_.end(),
            [&](const LoadedFont& lf) {
                return lf.family == ff.family && lf.weight == ff.weight &&
                       lf.italic == ff.italic;
            });
        if (alreadyLoaded) continue;

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
