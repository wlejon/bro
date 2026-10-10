#include "engine/engine.h"
#include "engine/app_runtime.h"
#include "engine/control.h"
#include "engine/frame_trace.h"
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
#include "engine/terminal_layers.h"
#include "dom/event_dispatch.h"
#include "util/asset_path.h"
#include "util/exe_dir.h"
#include "util/user_dirs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <thread>

#include "platform/window.h"
#include "platform/window_system.h"
#include "platform/desktop_platform.h"
#include "platform/dialogs.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "render/bidi.h"
#include "render/system_font_mgr.h"

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "api/fs_watch.h"  // Engine owns unique_ptr<FsWatcher>s; the ctor's unwind deletes them
#include "audio_inference/audio_inference.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#include "steam/steam_service.h"
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#endif
#include <broaudio/engine.h>
#include "canvas/canvas_scene.h"
#include "webgl/webgl2_context.h"
#include "dom/document.h"
#if BRO_WITH_A11Y
#include "a11y/a11y_bridge.h"
#endif
#include "dom/element.h"
#include "dom/event.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "util/log.h"
#include "util/time.h"

#include "render/vulkan_context.h"
#include "render/vulkan_swapchain.h"
#include "render/vulkan_presenter.h"
#include "webgl/webgl2_context.h"
#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace bro::engine {

namespace {

// The primary window: headless makes it hidden; a visible one is fitted into
// its display's work area.
platform::WindowConfig primaryWindowConfig(const EngineConfig& config, const GraphicsSettings& gfx,
                                           platform::GraphicsBackend backend) {
    platform::WindowConfig wc;
    wc.title = initialWindowTitle(config);
    wc.width = static_cast<uint32_t>(gfx.width);
    wc.height = static_cast<uint32_t>(gfx.height);
    wc.resizable = gfx.resizable;
    wc.vsync = gfx.vsync;
    wc.borderless = config.graphics.borderless;
    wc.backend = backend;
    wc.fitToWorkArea = true;
    wc.appId = config.appId;
    return wc;
}

}  // namespace

// The manifest's icon (SVG rasterized, or any bitmap broimage reads), else
// bro's own system/icon.png. SDL hands it to the window system: the window
// and taskbar icon on Windows, _NET_WM_ICON on X11, xdg-toplevel-icon on a
// Wayland compositor that has it (the others find the icon through the
// app_id's desktop entry, which `bro --install` writes).
void Engine::applyWindowIcon(const EngineConfig& config) {
    if (!window_) return;
    std::string path = appIconPath(config.manifest, config.appDir);
    if (path.empty()) {
        path = assetMounts_.resolve("/system/icon.png");
        std::error_code ec;
        if (path.empty() ||
            !std::filesystem::is_regular_file(std::filesystem::path(std::u8string(path.begin(), path.end())), ec))
            path = util::executableDir() + "/system/icon.png";
    }
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!loadIconPixels(path, 256, w, h, rgba)) {
        LOG_INFO("Window icon: could not load '%s'", path.c_str());
        return;
    }
    window_->setIconPixels(w, h, rgba.data());
}

Engine::Engine(const EngineConfig& config)
    : graphicsConfig_(config.graphics)
    , inputConfig_(config.input)
    , displayMode_(config.displayMode)
    , viewportWidth_(config.graphics.width)
    , viewportHeight_(config.graphics.height)
    , viewportScrollbar_(config.viewportScrollbar)
    , elementScrollbar_(config.elementScrollbar)
    , uiFrameIntervalMs_(config.graphics.maxFrameIntervalMs) {
    try {
        construct(config);
    } catch (...) {
        teardown();
        throw;
    }
}

void Engine::construct(const EngineConfig& config) {
    // Under DRM bro is the display server: no desktop window system, and so
    // no SDL video. Chosen before anything asks the platform layer a question.
    if (displayMode_ == DisplayMode::Drm)
        platform::selectWindowSystem(platform::WindowSystemKind::Drm);
    // A desktop window: Wayland on a Wayland session, else SDL.
    else if (displayMode_ == DisplayMode::Windowed)
        platform::selectDesktopWindowSystem();

    // Before the raster thread exists: it reads the pointer.
    terminalLayers_ = std::make_shared<TerminalLayers>();

    // The frame flight recorder and the agent control commands exist in
    // every mode; the socket is served from startControl().
    frameTrace_ = std::make_unique<FrameTrace>();
    control_ = std::make_unique<ControlServer>(*this);
    registerEngineControlCommands(*control_);
    registerControlInputCommands(*control_);
    registerControlRecordCommands(*control_);

    // The system font collection can take most of a second to build on some
    // machines; start it now so it overlaps window creation and page compile
    // rather than stalling the first layout (render/system_font_mgr.h).
    if (displayMode_ != DisplayMode::Server) render::prewarmSystemFontMgr();

    splashEnabled_ = config.showSplash;
    compiledApp_ = config.compiledApp;
    hostProvidesCompiledApp_ = config.hostProvidesCompiledApp;
    appDir_ = config.appDir;
    titleOverride_ = config.title;
    fallbackTitle_ = initialWindowTitle(config);
    installHostBindings_ = config.installHostBindings;
    installWorkerHostBindings_ = config.installWorkerHostBindings;
    initDevLoopConfig(config);
    desktopTrust_ = evaluateDesktopTrust(config.appDir, config.appId.empty() ? appIdFor(config.manifest, config.appDir)
                                                                             : config.appId,
                                         config.isShellApp, config.privilegedNamespaces);
    // JS runs on bronze's tiered default; BRO_JIT_TIER pins a tier to debug one.
    bronze_host::applyJitTierOverride();
    // CSS animations are Web Animations records (css_transitions.h).
    animationManager_.setWebAnimations(&webAnimationManager_);
    transitionManager_.setWebAnimations(&webAnimationManager_);

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
    // F5 belongs to the edit loop: an app that is not watched for source
    // changes (bro.json `"watch": false`, or BRO_WATCH=0) gets the key for
    // itself. The action still exists, unbound, for an app or user to bind.
    settings_->defineEngineAction("system_reload_app",
                                  watchSources_ ? std::vector<std::string>{"F5"} : std::vector<std::string>{});
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

    if (displayMode_ == DisplayMode::Server) {
        LOG_INFO("Server mode initializing (headless server tick loop)");
    }

    // Graphics initialization.
    // - Headless: an offscreen VulkanContext + presenter (no surface, no X11).
    // - Windowed: a platform::Window and SkiaRenderer, presented through a
    //   VulkanSwapchain + VulkanPresenter; without the GPU (useGPU = false)
    //   the window is a software one and frames are blitted to it on the CPU.
    // - Server: never initializes graphics.
    // Headless never reaches the desktop: tray, notifications, taskbar
    // progress and the bell record their state instead of touching the OS.
    //
    // The graphics come up in two halves with the page's script between them
    // (beginPageCompile): the GPU is the long pole (~160-190 ms on Windows:
    // the Vulkan loader and the GPU driver, instance, device, presenter,
    // Skia's context), and nothing before the page's first run needs it, so
    // the realm's host globals are installed and its first script compiled
    // while the device comes up rather than after.
    platform::desktop::setHeadless(displayMode_ == DisplayMode::Headless);
    GraphicsStartup gfxTiming;
    gfxTiming.startAtMs = sinceStartMs();
    const double gfxStart = util::currentTimeMs();
    render::SkiaGpu* headlessSkiaGpu = nullptr;
    std::string gpuInitError;
    std::thread gpuInit;
    double gpuMs = -1.0, gpuReadyAt = -1.0;
    struct JoinOnExit {
        std::thread& t;
        ~JoinOnExit() { if (t.joinable()) t.join(); }
    } joinGpuInit{gpuInit};
    if (displayMode_ == DisplayMode::Headless) {
        // The offscreen device needs no window, so it comes up on a thread
        // while this one creates the hidden window (~90 ms, SDL's video init,
        // which loads the same Vulkan loader for a SDL_WINDOW_VULKAN window),
        // installs the host globals and starts the page's compile. Nothing
        // else touches the context, the presenter or skiaGpu_ until the join.
        if (config.graphics.useGPU) {
            gpuInit = std::thread([this, &headlessSkiaGpu, &gpuInitError, &gpuMs, &gpuReadyAt] {
                const double t0 = util::currentTimeMs();
                struct Timed {
                    double t0;
                    double& out;
                    double& readyAt;
                    ~Timed() {
                        out = util::currentTimeMs() - t0;
                        readyAt = sinceStartMs();
                    }
                } timed{t0, gpuMs, gpuReadyAt};
                try {
                    render::VulkanContextConfig vkCfg;
                    vkCfg.headless = true;
                    vulkanContext_ = std::make_unique<render::VulkanContext>(vkCfg);
                    if (!vulkanContext_->init()) {
                        gpuInitError = "Headless Vulkan initialization failed (render::VulkanContext::init returned false)";
                        return;
                    }
                    vulkanPresenter_ = std::make_unique<render::VulkanPresenter>(*vulkanContext_);
                    if (!vulkanPresenter_->init()) {
                        gpuInitError = "Headless VulkanPresenter initialization failed (render::VulkanPresenter::init returned false)";
                        return;
                    }
                    headlessSkiaGpu = createSkiaGpu();
                } catch (const std::exception& e) {
                    gpuInitError = std::string("Headless Vulkan initialization failed: ") + e.what();
                }
            });
        }

        try {
            const auto backend = config.graphics.useGPU ? platform::GraphicsBackend::Vulkan
                                                        : platform::GraphicsBackend::Software;
            platform::WindowConfig wc = primaryWindowConfig(config, gfx, backend);
            wc.hidden = true;
            try {
                window_ = platform::createWindow(wc);
            } catch (const std::exception& e) {
                // When the video driver cannot create Vulkan windows (e.g. SDL's dummy driver
                // on headless Linux), fall back to a software window so headless still has a primary window.
                LOG_INFO("Headless window creation with %s backend failed (%s); falling back to Software backend",
                         backend == platform::GraphicsBackend::Vulkan ? "Vulkan" : "Software", e.what());
                wc.backend = platform::GraphicsBackend::Software;
                window_ = platform::createWindow(wc);
            }

            const auto& wcfg = config.graphics;
            if (wcfg.alwaysOnTop) window_->setAlwaysOnTop(true);
            if (wcfg.minWidth > 0 || wcfg.minHeight > 0)
                window_->setMinimumSize(wcfg.minWidth, wcfg.minHeight);
            if (wcfg.maxWidth > 0 || wcfg.maxHeight > 0)
                window_->setMaximumSize(wcfg.maxWidth, wcfg.maxHeight);
        } catch (const std::exception& e) {
            LOG_INFO("Headless window creation skipped: %s", e.what());
            window_.reset();
        }
        gfxTiming.windowMs = util::currentTimeMs() - gfxStart;
    } else if (displayMode_ == DisplayMode::Windowed) {
        {
            const auto backend = config.graphics.useGPU ? platform::GraphicsBackend::Vulkan
                                                        : platform::GraphicsBackend::Software;
            window_ = platform::createWindow(primaryWindowConfig(config, gfx, backend));

            const auto& wcfg = config.graphics;
            if (wcfg.alwaysOnTop) window_->setAlwaysOnTop(true);
            if (wcfg.minWidth > 0 || wcfg.minHeight > 0)
                window_->setMinimumSize(wcfg.minWidth, wcfg.minHeight);
            if (wcfg.maxWidth > 0 || wcfg.maxHeight > 0)
                window_->setMaximumSize(wcfg.maxWidth, wcfg.maxHeight);
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

            applyWindowIcon(config);
            int ww = 0, wh = 0;
            window_->getSize(ww, wh);
            if (ww > 0 && wh > 0) {
                viewportWidth_ = ww;
                viewportHeight_ = wh;
            }
        }
        gfxTiming.windowMs = util::currentTimeMs() - gfxStart;
    } else if (displayMode_ == DisplayMode::Drm) {
        initDrm(config);
    } else {
        renderer_ = std::make_unique<render::RasterRenderer>();
    }

    auto initAudio = [&] {
        audioEngine_ = std::make_unique<broaudio::Engine>();
        if (displayMode_ == DisplayMode::Windowed || config.realAudio) {
            // The app's identity names its streams in the system mixer (the
            // PipeWire node, SDL's app name), as it names the window.
            broaudio::AudioDeviceConfig audioCfg;
            audioCfg.appId = config.appId;
            audioCfg.appName = config.manifest.name.empty() ? config.appId : config.manifest.name;
            if (!audioEngine_->init(audioCfg)) audioEngine_->initHeadless();
        } else {
            audioEngine_->initHeadless();
        }
        SceneAudioSync::install(audioEngine_.get());
    };

    // The page's first script compiles on its own thread from here; its run
    // (initAppRealm) takes the program. Windowed, the device below comes up
    // on this thread meanwhile; headless, it is already coming up on its own.
    // Audio first: AudioContext binds the engine's audio engine when the host
    // globals are installed. BRO_STARTUP_OVERLAP=0 keeps the order bro had
    // (device, audio, then the page compiled when it runs), to measure what
    // the overlap is worth or to rule it out.
    const char* overlapEnv = std::getenv("BRO_STARTUP_OVERLAP");
    const bool overlapPage = (displayMode_ == DisplayMode::Headless || displayMode_ == DisplayMode::Windowed) &&
                             !(overlapEnv && std::string(overlapEnv) == "0");
    if (overlapPage) {
        initAudio();
        beginPageCompile();
    }

    if (displayMode_ == DisplayMode::Headless) {
        if (gpuInit.joinable()) gpuInit.join();
        if (!gpuInitError.empty()) throw std::runtime_error(gpuInitError);
        if (config.graphics.useGPU) {
#if BRO_WITH_3D
            scene::SceneRenderer::setDefaultVulkanContext(vulkanContext_.get());
#endif
            LOG_INFO("Engine: Headless Vulkan initialized successfully");
        }
        // Without the GPU, Skia draws the same layers on the CPU and the
        // frame composites on the CPU: one pipeline either way.
        auto skia = std::make_unique<render::SkiaRenderer>();
        if (vulkanPresenter_) skia->setGpu(headlessSkiaGpu);
        renderer_ = std::move(skia);
    } else if (displayMode_ == DisplayMode::Windowed) {
        // The device is chosen against the window's surface (VulkanContext::
        // init), and the surface stays on this thread (a Cocoa view must be
        // made on the main thread), so the device comes up here while the
        // page's compile runs on its own.
        const double gpuStart = util::currentTimeMs();
        {
            renderer_ = render::createRenderer();
            if (!renderer_) {
                throw std::runtime_error("Failed to create renderer");
            }

            if (config.graphics.useGPU) {
                render::VulkanContextConfig vkCfg;
                vkCfg.headless = false;
                vulkanContext_ = std::make_unique<render::VulkanContext>(vkCfg);
                if (!vulkanContext_->init(window_.get())) {
                    throw std::runtime_error("Windowed Vulkan context initialization failed");
                }
                vulkanSwapchain_ = std::make_unique<render::VulkanSwapchain>(*vulkanContext_, window_.get(), gfx.vsync);
                if (!vulkanSwapchain_->init()) {
                    throw std::runtime_error("Windowed Vulkan swapchain initialization failed");
                }
                vulkanPresenter_ = std::make_unique<render::VulkanPresenter>(*vulkanContext_, *vulkanSwapchain_);
                if (!vulkanPresenter_->init()) {
                    throw std::runtime_error("Windowed Vulkan presenter initialization failed");
                }
                vulkanPresenter_->setCapturePresents(capturePresentsRequested());
#if BRO_WITH_3D
                scene::SceneRenderer::setDefaultVulkanContext(vulkanContext_.get());
#endif
                if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get()))
                    skia->setGpu(createSkiaGpu());
                gpuMs = util::currentTimeMs() - gpuStart;
                gpuReadyAt = sinceStartMs();
            }
        }
    }
    if (displayMode_ == DisplayMode::Headless || displayMode_ == DisplayMode::Windowed) {
        gfxTiming.gpuMs = gpuMs;
        gfxTiming.readyAtMs = gpuReadyAt;
        gfxTiming.totalMs = util::currentTimeMs() - gfxStart;
        noteGraphicsStartup(gfxTiming);
    }
    if (!overlapPage) initAudio();

    deviceScale_.configured = config.deviceScaleFactor;
    updateDeviceScale();
    if (deviceScale_.render != 1.0f || deviceScale_.ratio != 1.0f)
        LOG_INFO("Device scale %.2f (devicePixelRatio %.2f): %dx%d CSS px, drawable %dx%d",
                 deviceScale_.render, deviceScale_.ratio, viewportWidth_, viewportHeight_,
                 deviceScale_.drawableW, deviceScale_.drawableH);

    if (displayMode_ == DisplayMode::Headless) {
        virtualTime_ = util::currentTimeMs();
        engineNowMs_ = virtualTime_;
        // A shell under test can host real clients: the DRM shell host's
        // compositor, window manager and input router, with no seat or KMS.
        const char* hostComp = std::getenv("BRO_HEADLESS_COMPOSITOR");
        if (hostComp && std::string(hostComp) == "1" && isShellApp())
            startShellCompositor(viewportWidth_, viewportHeight_, "", /*xwayland=*/false);
    }

    audioInference_ = std::make_unique<AudioInference>();
    if (displayMode_ == DisplayMode::Windowed)
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
            if ((key == "vsync" || key == "*") && window_) {
                window_->setVSync(g.vsync);
                if (vulkanSwapchain_) vulkanSwapchain_->setVSync(g.vsync);
            }
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
        eventLoop_ = platform::windowSystem().createEventLoop();
    }

    // Native dialogs parent on the window and tick timers while they are up.
    // Only a windowed run has someone to answer them; headless and server
    // answer themselves (see platform::Dialogs) instead of blocking on a
    // window nobody sees. Set before any script runs: the first alert() an
    // app's boot script reaches must already know there is no one to ask.
    platform::Dialogs::setWindow(window_.get());
    platform::Dialogs::setInteractive(displayMode_ == DisplayMode::Windowed);
    platform::Dialogs::setTickCallback([this]() { tickTimersOnly(); });

    manifest_ = AppLoader::loadApp(appDir_, &assetMounts_);
    util::setAssetPathContext(manifest_.basePath, &assetMounts_);
    drawTraversal_->setViewport(viewportWidth_, viewportHeight_, 0);

    if (displayMode_ != DisplayMode::Server) {
        initSystemPanels();
    }

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
    initAppWatcher();

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
    std::string html;
    if (!appHtmlOverride_.empty()) {
        html = appHtmlOverride_;
        manifest_.scripts.clear();
        manifest_.stylePaths.clear();
    } else if (!manifest_.htmlPath.empty()) {
        html = AppLoader::loadFile(manifest_.htmlPath);
    }
    if (html.empty()) {
        if (!manifest_.scripts.empty()) {
            html = "<!DOCTYPE html><html><head><title>Bro</title></head><body></body></html>";
        } else {
            throw std::runtime_error("Failed to load index.html from " + appDir_);
        }
    }

    drawTraversal_->setBasePath(manifest_.basePath);
    drawTraversal_->setViewport(contentWidth(), contentHeight(), 0);

    // Each linked sheet stays its own sheet, based at its own directory, so
    // its relative url()s resolve against it rather than the document.
    std::vector<dom::Document::AuthorSheet> authorStyles;
    for (auto& cssPath : manifest_.stylePaths) {
        std::string css = AppLoader::loadFile(cssPath);
        if (!css.empty()) {
            authorStyles.push_back(
                {std::move(css), std::filesystem::path(cssPath).parent_path().string()});
        }
    }

    // <template> elements go through gumbo like everything else: the tree
    // builder puts their children in the inert content fragment
    // (Document::buildTreeFromGumbo), so the app document needs no pre-pass.
    document_ = std::make_unique<dom::Document>();
    document_->setBasePath(manifest_.basePath);
    document_->setMediaViewport(static_cast<float>(contentWidth()),
                                static_cast<float>(contentHeight()));
    document_->setMediaColorScheme(effectiveColorScheme());
    document_->setMediaResolution(deviceScale_.ratio);
    document_->cascade().setImportResolver(
        [this](const std::string& url, const std::string& base, std::string& importedBase) {
        // Relative to the importing sheet (its directory), else the document.
        std::string path = AppLoader::resolvePath(
            base.empty() ? document_->basePath() : base, url, &assetMounts_);
        std::string css = AppLoader::loadFile(path);
        if (css.empty()) {
            LOG_WARN("@import: failed to load '%s' (resolved to '%s')",
                     url.c_str(), path.c_str());
        }
        importedBase = std::filesystem::path(path).parent_path().string();
        return css;
    });
    document_->parse(html, authorStyles, kDefaultStyles);

    if (window_) {
        // bro.json's "title" fixes the title; otherwise the window follows
        // the page's title, live, and shows the app's name while it has none.
        auto apply = [this](const std::string& t) {
            if (window_) window_->setTitle(t.empty() ? fallbackTitle_ : t);
        };
        if (!titleOverride_.empty()) {
            window_->setTitle(titleOverride_);
        } else {
            apply(document_->title());
            document_->setTitleListener(apply);
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
        updateDocumentHeight();
        syncIframes();
#if BRO_WITH_A11Y
        a11yBridge_ = std::make_unique<a11y::AccessibilityBridge>();
        a11yBridge_->initialize(document_.get());
#endif
    }

    // bro-server runs its server script, not the page: index.html's scripts
    // are written for a renderer the server does not have, and a throw there
    // is no failure of the server's. A script-entry app (no index.html; its
    // server.js / main.js is the entry, htmlPath empty) still runs here.
    const bool serverSkipsPage = displayMode_ == DisplayMode::Server &&
                                 !manifest_.htmlPath.empty();
    if (serverSkipsPage && !manifest_.scripts.empty()) {
        LOG_INFO("bro-server: not running the %zu page script(s) of '%s'",
                 manifest_.scripts.size(), manifest_.htmlPath.c_str());
    }
    if (!manifest_.scripts.empty() && !serverSkipsPage) {
        // Classic scripts share one global scope, so they stay ONE unit, in
        // document order, named for the page. Module scripts are deferred on
        // the web — they run after the classic ones, in document order — and
        // each is its own module with its own URL: an external one is named
        // for its file, so import.meta.url and its relative imports resolve
        // from where it lives rather than from index.html; an inline one is
        // named for the page, whose URL is its base. The realm's module
        // registry evaluates a module two scripts import once.
        const std::string combinedScripts = combinedClassicScripts();
        if (!combinedScripts.empty()) {
            if (!bro::bronze_host::evalAppScript(*this, combinedScripts, manifest_.htmlPath)) {
                setTestFailure(true);
            }
        }
        // An external module script IS the module instance for its file: it is
        // published under its path, so a later `import` of that file — from a
        // headless driver script, or another module script — binds the
        // instance that ran rather than running the file again. Two tags with
        // one src are one module, evaluated once (HTML's module map).
        std::vector<std::string> ranModuleFiles;
        for (const auto& script : manifest_.scripts) {
            if (!script.isModule) continue;
            if (!script.isInline()) {
                if (std::find(ranModuleFiles.begin(), ranModuleFiles.end(), script.path) !=
                    ranModuleFiles.end()) {
                    continue;
                }
                ranModuleFiles.push_back(script.path);
            }
            std::string code = script.isInline() ? script.code : AppLoader::loadFile(script.path);
            if (code.empty()) continue;
            const std::string& name = script.isInline() ? manifest_.htmlPath : script.path;
            if (!bro::bronze_host::evalAppScript(*this, code, name,
                                                 /*moduleFile=*/!script.isInline())) {
                setTestFailure(true);
            }
        }
    }

    // A compile begun at launch that no run took (nothing should be left).
    bronze_host::discardAppScriptCompile();

    if (!hostProvidesCompiledApp_) {
        dispatchDocumentReadyEvents();
    }
}

std::string Engine::combinedClassicScripts() const {
    std::string combined;
    for (const auto& script : manifest_.scripts) {
        if (script.isModule) continue;
        std::string code = script.isInline() ? script.code : AppLoader::loadFile(script.path);
        if (!code.empty()) {
            if (!combined.empty()) combined += "\n;\n";
            combined += code;
        }
    }
    return combined;
}

bool Engine::firstPageScriptUnit(PageScriptUnit& out) const {
    if (displayMode_ == DisplayMode::Server && !manifest_.htmlPath.empty()) return false;
    std::string classic = combinedClassicScripts();
    if (!classic.empty()) {
        out = {std::move(classic), manifest_.htmlPath, false};
        return true;
    }
    for (const auto& script : manifest_.scripts) {
        if (!script.isModule) continue;
        std::string code = script.isInline() ? script.code : AppLoader::loadFile(script.path);
        if (code.empty()) continue;
        out = {std::move(code), script.isInline() ? manifest_.htmlPath : script.path, !script.isInline()};
        return true;
    }
    return false;
}

// Only the FIRST unit compiles ahead: it is compiled against the realm as it
// stands before any of the page's scripts ran, which is the realm its compile
// at run time would see too. A later unit may import a module an earlier one
// published, so its compile has to wait for that run.
void Engine::beginPageCompile() {
    if (!appHtmlOverride_.empty()) return;
    manifest_ = AppLoader::loadApp(appDir_, &assetMounts_);
    util::setAssetPathContext(manifest_.basePath, &assetMounts_);
    PageScriptUnit unit;
    if (!firstPageScriptUnit(unit)) return;
    bronze_host::startAppScriptCompile(*this, unit.code, unit.name, unit.moduleFile);
}

void Engine::dispatchDocumentReadyEvents() {
    if (documentReadyState_ == "complete") return;
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
    noteDocumentLoaded();  // the next presented frame is the page's first

    mediaEventsArmed_ = true;
}

webgl::WebGL2RenderingContext* Engine::createWebGL2Context(dom::Element* canvas) {
    if (!vulkanContext_) return nullptr;

    if (canvas && canvas->webglContext()) {
        return static_cast<webgl::WebGL2RenderingContext*>(canvas->webglContext());
    }

    // The drawing buffer is the canvas's width/height attributes when it has
    // them (what syncWebGLCanvasSizes keeps it at every frame after), else
    // its laid-out box, else the viewport. Starting from the box or viewport
    // when the attributes say otherwise left a `canvas.width = 64` canvas
    // with a 1920x1080 buffer until the next frame, so a draw + readPixels
    // in the same turn saw a 64-pixel corner of a stretched image.
    int cw = viewportWidth_, ch = viewportHeight_;
    if (canvas) {
        auto& box = canvas->layoutBox();
        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
        const auto attrInt = [canvas](const char* name, int fallback) {
            const std::string& v = canvas->getAttribute(name);
            const int n = v.empty() ? 0 : std::atoi(v.c_str());
            return n > 0 ? n : fallback;
        };
        cw = attrInt("width", cw);
        ch = attrInt("height", ch);
    }
    auto ctx2 = std::make_unique<webgl::WebGL2RenderingContext>(cw, ch, *vulkanContext_);
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
    // Seed the bitmap size only from width/height attributes that were
    // assigned before getContext() ran. A canvas WITHOUT them stays
    // layout-driven (intrinsic 0 → CanvasScene::queryLayoutWidth falls
    // through to the content box), which is what ctx.canvasWidth and the
    // arcade apps' full-viewport canvases depend on. Pinning 300x150 here
    // would stretch every attribute-less canvas to its CSS box.
    const std::string wAttr = canvas->getAttribute("width");
    const std::string hAttr = canvas->getAttribute("height");
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

    // A canvas in a sub-document (secondary window, <iframe>, system panel)
    // is parked in THAT document's list, unbound: those documents composite
    // by blitting the scene inline from their own draw path, which hands the
    // scene the Skia context it should raster on at blit time (sub_document.cpp
    // replayBufferWithInlineCanvas, system_panels.cpp drawSystemPanelDoc).
    dom::Document* doc = canvas->document();
    if (doc) {
        auto parkInSubDoc = [&](std::vector<std::unique_ptr<canvas::CanvasScene>>& list) {
            canvasSceneRegistry_[canvasScene->sceneId()] = csPtr;
            list.push_back(std::move(canvasScene));
        };
        for (auto& h : windowHosts_) {
            if (h && h->document.get() == doc) {
                parkInSubDoc(h->canvasScenes);
                return csPtr;
            }
        }
        for (auto& d : iframeDocs_) {
            if (d && d->document.get() == doc) {
                parkInSubDoc(d->canvasScenes);
                return csPtr;
            }
        }
        for (auto& s : systemDocs_) {
            if (s.document.get() == doc) {
                parkInSubDoc(s.canvasScenes);
                return csPtr;
            }
        }
    }

    // The main document: the one path that registers a scene the compositor
    // rasterizes — the registry and the raster binding. Registering by hand
    // here left every 2D canvas whose getContext ran after boot unbound in
    // windowed mode, so it painted nothing; the scene-context factory below
    // goes through addCanvasScene too.
    addCanvasScene(std::move(canvasScene));
    return csPtr;
}

#if BRO_WITH_3D
static void severSceneGraphLink(dom::Element* el);
#endif

scene::SceneGraph* Engine::createSceneContext(dom::Element* canvas) {
#if !BRO_WITH_3D
    (void)canvas;
    return nullptr;
#else
    if (!vulkanContext_) return nullptr;

    // A CanvasScene for `canvas` — the 2D layer a graph's sprites and shapes
    // draw into — registered with the compositor and linked to the element.
    auto bindSceneCanvas = [this](dom::Element* canvas) -> canvas::CanvasScene* {
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
        return csPtr;
    };

    if (canvas && canvas->sceneGraph()) {
        if (auto* existing = sceneGraphForElement(canvas)) return existing;
    }
    if (!canvas) return nullptr;

    auto graph = std::make_unique<scene::SceneGraph>();
    graph->setPhysicsWorld(physicsWorld_.get());
    graph->setCanvasSize(viewportWidth_, viewportHeight_);
    graph->setDeviceScale(deviceScale_.render);
    auto* graphPtr = graph.get();
    graphPtr->setGizmoProvider([this](scene::SceneGraph* g) {
        return gizmo_ ? gizmo_->meshesForRender(g)
                      : std::vector<scene::MeshNode*>{};
    });
    // The one path that binds a graph to a canvas — at creation below, and
    // for SceneGraph.attachTo / detach / keepAlive afterwards.
    graphPtr->setCanvasRebinder([this, graphPtr, bindSceneCanvas](void* targetPtr) -> bool {
        auto* target = static_cast<dom::Element*>(targetPtr);
        auto it = std::find_if(sceneGraphs_.begin(), sceneGraphs_.end(),
            [graphPtr](const SceneGraphEntry& e) { return e.graph.get() == graphPtr; });
        if (it == sceneGraphs_.end()) return false;
        dom::Element* old = liveElementOf(*it);
        if (target && target == old && !graphPtr->parked()) return true;
        if (target && target->sceneGraph()) return false;  // another graph's canvas

        // Unbind: the old canvas stops compositing the graph, and the old
        // CanvasScene is retired (the frame loop's detached sweep drops it).
        // Nothing the nodes hold on the GPU is touched.
        severSceneGraphLink(old);
        if (auto* oldCs = graphPtr->canvasScene()) {
            if (old && old->canvasScene() == oldCs) old->setCanvasScene(nullptr);
            oldCs->onElementFinalized();
        }
        graphPtr->setCanvasScene(nullptr);
        graphPtr->setLayerCallback({});
        it->element = nullptr;
        it->document = nullptr;
        it->elementId = 0;
        graphPtr->setParked(true);
        if (!target) return true;

        auto* csPtr = bindSceneCanvas(target);
        graphPtr->setCanvasScene(csPtr);
        int cw = graphPtr->canvasWidth(), ch = graphPtr->canvasHeight();
        const auto& box = target->layoutBox();
        if (box.contentRect.width > 0) cw = static_cast<int>(box.contentRect.width);
        if (box.contentRect.height > 0) ch = static_cast<int>(box.contentRect.height);
        graphPtr->setCanvasSize(cw, ch);
        target->setSceneGraph(graphPtr);
        graphPtr->setLayerCallback([target](const render::LayerImage& layer) {
            target->setSceneLayerReady(static_cast<bool>(layer));
        });
        it->element = target;
        it->document = target->document();
        it->elementId = target->nodeId();
        graphPtr->setParked(false);
        return true;
    });
    sceneGraphs_.push_back({std::move(graph), nullptr, nullptr, 0});
    graphPtr->setParked(true);
    graphPtr->rebindCanvas(canvas);
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
    el->setSceneLayerReady(false);
}

void Engine::pruneDetachedSceneGraphs() {
    // A keepAlive graph whose canvas left the DOM (or died) is parked, not
    // destroyed: its GPU state waits for SceneGraph.attachTo on a new canvas.
    // Parked entries have no element, which the sweep below skips.
    for (auto& sg : sceneGraphs_) {
        if (!sg.element || !sg.graph || !sg.graph->keepAlive()) continue;
        dom::Element* el = liveElementOf(sg);
        bool detached = !el;
        if (el) {
            auto* n = el;
            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
            detached = n->tagName() != "html" && n->tagName() != "HTML";
        }
        if (detached) sg.graph->rebindCanvas(nullptr);
    }
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

        // src is relative to the sheet that declared the rule (its directory,
        // through @import too); a sheet with no base is the document's.
        std::string path = AppLoader::resolvePath(
            ff.baseUrl.empty() ? basePath : ff.baseUrl, ff.src, &assetMounts_);

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

namespace bro::engine {

// Skia's GPU context on the engine's device. Null — Skia then draws on the
// CPU and the presenter uploads its pixels — without Vulkan, when Skia cannot
// run on the device, or with BRO_SKIA_GPU=0 (for comparing the two).
render::SkiaGpu* Engine::createSkiaGpu() {
    if (!vulkanContext_) return nullptr;
    if (const char* v = std::getenv("BRO_SKIA_GPU"); v && std::string_view(v) == "0") {
        LOG_INFO("Engine: BRO_SKIA_GPU=0, Skia draws on the CPU");
        return nullptr;
    }
    auto gpu = std::make_unique<render::SkiaGpu>(*vulkanContext_);
    if (!gpu->init()) return nullptr;
    skiaGpu_ = std::move(gpu);
    return skiaGpu_.get();
}

} // namespace bro::engine
