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
#include "js/custom_elements.h"
#include "js/webgl2_bindings.h"
#include "js/image_bindings.h"
#include "js/imagebitmap_bindings.h"
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
#include "js/gpu_bindings.h"
#include "js/diffusion_bindings.h"
#include "js/lm_bindings.h"
#include "js/stt_bindings.h"
#include "js/tts_bindings.h"
#include "js/diar_bindings.h"
#include "js/rave_bindings.h"
#include "js/triposplat_bindings.h"
#include "js/vision_bindings.h"
#include "js/wake_bindings.h"
#include "js/gesture_bindings.h"
#include "js/kws_bindings.h"
#include "js/listen_bindings.h"
#include "js/listen_host.h"
#include "js/sense_bindings.h"
#include "js/mic_bindings.h"
#include "js/terrain_bindings.h"
#include "js/tile_bindings.h"
#include "js/net_bindings.h"
#include "js/steam_bindings.h"
#include "js/server_bindings.h"
#include "js/headless_bindings.h"

#include "physics/physics_world.h"
#include "audio_inference/audio_inference.h"
#include "net/net_service.h"
#include "steam/steam_service.h"
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
    inputConfig_.scrollSpeed = inp.scrollSpeed;
    inputConfig_.doubleClickThresholdMs = inp.doubleClickThresholdMs;
    inputConfig_.doubleClickDistancePx = inp.doubleClickDistancePx;
    inputConfig_.overlayToggleKey = inp.overlayToggleKey;

    // === Common JS runtime initialization ===

    // 4. JS runtime + bindings
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

    // bro.gpu (runtime backend probe via brotensor). Always present — reports
    // whether the ML loaders below will default to a GPU or fall back to CPU,
    // so apps can warn before loading a large model on CPU.
    js::installGpuBindings(jsRuntime_->getContext());

    // bro.tensor (GPU tensor + ops via brotensor sibling). Real bindings when
    // a backend is enabled at configure time; stub `{ available: false }` otherwise.
    js::installTensorBindings(jsRuntime_->getContext());

    // bro.diffusion (diffusion-model inference via brodiffusion sibling).
    // Always real — brodiffusion's CPU backend is always built. Main thread
    // drives the step-wise inspection API; workers install the same binding
    // for fast full generation.
    js::installDiffusionBindings(jsRuntime_->getContext());

    // bro.lm (Qwen3 LLM inference via brolm sibling). Always real — brolm's
    // CPU backend is always built. GGUF quant weights (Q4_K/Q6_K/Q8_0)
    // dispatch through GPU-only fused-dequant matmuls and throw at first
    // forward on a CPU-only build.
    js::installLmBindings(jsRuntime_->getContext());

    // bro.stt (Whisper + Parakeet STT via brosoundml + brolm siblings). GPU by
    // default; audio must be 16 kHz mono FP32 — callers resample upstream.
    js::installSttBindings(jsRuntime_->getContext());

    // bro.tts (Kokoro-82M TTS via brosoundml sibling). CPU-only today; emits
    // mono 24 kHz FP32. G2P is the caller's responsibility — Kokoro takes
    // already-tokenized phoneme ids.
    js::installTtsBindings(jsRuntime_->getContext());

    // bro.diar (streaming Sortformer speaker diarization via brosoundml
    // sibling). GPU by default; 16 kHz mono FP32 in, per-frame 4-speaker
    // activity probabilities out. Offline diarize() + streaming sessions.
    js::installDiarBindings(jsRuntime_->getContext());

    // bro.rave (RAVE neural audio autoencoder via brosoundml sibling). GPU by
    // default; encode/decode a waveform through a PCA-sorted latent for morphing.
    js::installRaveBindings(jsRuntime_->getContext());

    // bro.vision (vision-ML inference via brovisionml sibling). GPU by default;
    // SAM segmentation + Depth-Anything depth (+ the ControlNet annotators).
    // Heavy ops run on a background thread when given an onReady/onDone callback.
    js::installVisionBindings(jsRuntime_->getContext());

    // bro.triposplat (single-image -> 3D Gaussian Splat). Composition layer over
    // DINOv3 (brovisionml) + Flux.2 VAE / flow DiT / octree decoder (brodiffusion);
    // emits a Gaussian cloud for the scene GaussianSplatNode. GPU by default.
    js::installTriposplatBindings(jsRuntime_->getContext());

    // Terrain bindings (infinite voxel terrain system — all modes)
    js::TerrainBindings::install(jsRuntime_->getContext());

    // Tile-world bindings (scene.createTileWorld — chunked tile grid meshing)
    js::TileBindings::install(jsRuntime_->getContext());

    // Network service + bindings (all modes). NetService owns GNS on its own
    // thread; bindings hold a per-context subscriber that polls each frame.
    netService_ = std::make_unique<net::NetService>();
    js::NetBindings::install(jsRuntime_->getContext(), netService_.get());

    // Steam service + bindings (all modes). Always-present probe like bro.gpu:
    // in a stub build (BRO_WITH_STEAM=OFF) the service is inert and
    // bro.steam.available is false. When enabled, SteamService owns the
    // Steamworks API on its own thread; bindings hold a per-context subscriber
    // that polls each frame.
    steamService_ = std::make_unique<steam::SteamService>();
    js::SteamBindings::install(jsRuntime_->getContext(), steamService_.get());

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
    if (displayMode_ == DisplayMode::Windowed || config.realAudio) {
        audioEngine_->init();
    } else {
        audioEngine_->initHeadless();
    }
    js::AudioBindings::install(jsRuntime_->getContext(), audioEngine_.get());

    // Audio-inference subsystem: owns the background worker thread that runs
    // audio-driven NN models (wake word today) off the audio thread and off the
    // main thread. Threaded in Windowed/Server; pumped inline in Headless for
    // deterministic tests — the same parity pattern as physicsWorld_ above.
    audioInference_ = std::make_unique<AudioInference>();
    if (displayMode_ != DisplayMode::Headless)
        audioInference_->startThread();

    // bro.wake (brosoundml::WakeWord — streaming wake-word detection driven
    // by broaudio's low-latency mic tap, inference run by audioInference_).
    // Must follow audio + inference init so the binding can wire both in; the
    // JS surface itself is inert until the app calls bro.wake.listen().
    js::installWakeBindings(jsRuntime_->getContext(), audioEngine_.get(),
                            audioInference_.get());

    // The shared listen host: ONE raw (no-AGC) mic tap + ring + inference
    // task driving a brosoundml::ListenBus that bro.kws's spotter and
    // bro.sense's hub join as members — one PCEN feature pass, one PhonemeNet
    // forward, N listeners. (bro.wake stays on its own AGC'd tap above until
    // retrained AGC-free.) Inert until a member attaches.
    js::installListenHost(audioEngine_.get(), audioInference_.get());

    // bro.listen — the shared stream's own JS surface (opt-in raw-audio
    // retention for replay/scrub by frame range). Inert until bro.listen.retain.
    js::installListenBindings(jsRuntime_->getContext());

    // bro.kws (brosoundml::PhonemeSpotter — open-vocabulary streaming keyword
    // spotting). A listen-host member; result delivery stays its own
    // (SPSC event ring -> tickKws). Inert until the app calls bro.kws.load().
    js::installKwsBindings(jsRuntime_->getContext(), audioEngine_.get(),
                           audioInference_.get());

    // bro.sense (brosoundml::SensorHub — the tier-0 acoustic sensor bus:
    // model-free per-frame level/VAD, onset, and tonality sensors). A
    // listen-host member with no result ring at all — the hub's lock-free
    // snapshot IS the delivery, polled via bro.sense.snapshot(). Inert until
    // bro.sense.start().
    js::installSenseBindings(jsRuntime_->getContext(), audioEngine_.get(),
                             audioInference_.get());

    // bro.gesture (brosoundml::GestureSpotter — tier-0 non-speech gesture
    // matching: enrolled rhythm/tone gestures over the shared SensorHub stream,
    // for the clicks/whistles the speech model can't represent). A listen-host
    // member with its own SPSC ring -> tickGesture. Inert until enroll/listen.
    js::installGestureBindings(jsRuntime_->getContext(), audioEngine_.get(),
                               audioInference_.get());

    // bro.mic (general live-mic chunk consumer — the worked example of
    // broaudio's chunkFrames feature). Same shape as wake: a mic tap on the
    // audio thread, drained on the main thread. Inert until bro.mic.start().
    js::installMicBindings(jsRuntime_->getContext(), audioEngine_.get());

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

    // Engine gizmo (bro.gizmo.*) — translate/rotate/scale handles with
    // mouse-driven picking and drag interaction.
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
    js::ImageBitmapBindings::install(jsRuntime_->getContext());
    js::VideoBindings::install(jsRuntime_->getContext(), manifest_.basePath);
    js::SceneBindings::setAppContext(manifest_.basePath, &assetMounts_);
    js::TileBindings::setAppContext(manifest_.basePath, &assetMounts_);
    js::setDiffusionAppContext(manifest_.basePath, &assetMounts_);
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
        double now = (displayMode_ == DisplayMode::Headless)
                         ? virtualTime_
                         : util::currentTimeMs();
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

    // Headless: flush an initial frame. Style + layout already ran above
    // (step 10a, before the load dispatch); flush() re-layouts only if a load
    // handler dirtied the document, then rasterizes.
    if (displayMode_ == DisplayMode::Headless) {
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
