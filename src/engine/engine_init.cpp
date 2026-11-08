#include "engine/engine.h"
#include "engine/app_runtime.h"
#include "engine/control.h"
#include "engine/dom_undo.h"
#include "engine/frame_trace.h"
#include "engine/frame_presenter.h"
#include "engine/gizmo.h"
#include "engine/layout_pipeline.h"
#include "engine/inspector_highlight.h"
#include "engine/key_mapping.h"
#include "dom/element_geometry.h"
#include "layout/box.h"
#include "layout/layout_node_adapter.h"
#include "layout/svg_image.h"
#include "engine/overflow.h"
#include "engine/overlay.h"
#include "engine/replaced_elements.h"
#include "engine/default_styles.h"
#include "engine/scene_audio_sync.h"
#include "engine/terminal_layers.h"
#include "dom/event_dispatch.h"
#include "util/asset_path.h"
#include "util/exe_dir.h"
#include "util/model_dirs.h"
#include "util/subresource_access.h"
#include "util/user_dirs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <chrono>
#include <thread>

#include "platform/window.h"
#include "platform/window_system.h"
#include "platform/desktop_platform.h"
#include "platform/desktop_hotkeys.h"
#include "platform/dialogs.h"
#include "platform/event_loop.h"
#include "render/renderer.h"
#include "render/raster_renderer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include <include/core/SkData.h>
#include "render/bidi.h"
#include "render/kernel_shader.h"
#include "render/system_font_mgr.h"

#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "bronze_host/app_aot.h"
#include "bronze_host/eval.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/script_pump.h"
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

#if BRO_WITH_3D
// (Declared in scene_vk_target_format.h, which needs the scene's Vulkan
// function table.)
namespace bro::scene::vk::pipeline_async {
void setAsync(bool on);
}
#endif

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
    wc.transparent = config.graphics.transparent;
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
    , desktopShell_(config.desktopShell)
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
    overlayMgr_ = std::make_unique<OverlayManager>();
    editUndo_ = std::make_unique<DomUndoHistories>();
    // Before the engine's threads exist, so they share a few arenas.
    configureHeap();
    // This thread runs the page: natives that block on it pump it
    // (util/page_pump.h).
    installPagePump();
    // Kernel compiles load from disk when unchanged (docs/code-cache.md);
    // the engine's own scene stages start compiling now, on threads of their
    // own, for the first scene's setup to find built.
    render::configureKernelCache();
#if BRO_WITH_3D
    if (config.displayMode != DisplayMode::Server) scene::SceneRenderer::prewarmBuiltinShaders();
#endif
    // Before any image decodes: rasterizing an SVG image paints it with the
    // layout's SVG traversal.
    layout::installSvgImagePainter();
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
    // The corner status panel (shader_status.cpp) in every window; in headless
    // only with the splash (--splash), so screenshots stay clean by default.
    statusEnabled_ = displayMode_ == DisplayMode::Windowed || displayMode_ == DisplayMode::Drm ||
                     (displayMode_ == DisplayMode::Headless && splashEnabled_);
    compiledApp_ = config.compiledApp;
    hostProvidesCompiledApp_ = config.hostProvidesCompiledApp;
    aotDir_ = config.aotDir;
    aotBuildDir_ = config.aotBuildDir;
    appDir_ = config.appDir;
    titleOverride_ = config.title;
    fallbackTitle_ = initialWindowTitle(config);
    installHostBindings_ = config.installHostBindings;
    installWorkerHostBindings_ = config.installWorkerHostBindings;
    initDevLoopConfig(config);
    if (!config.manifestError.empty()) throw std::runtime_error(config.manifestError);
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

    // === What the app may do (app_permissions.h) ===
    // Before any realm exists: every check from the first script on reads it.
    {
        PermissionsSetup perms;
        perms.appDir = config.appDir;
        perms.appId = config.appId.empty() ? appIdFor(config.manifest, config.appDir) : config.appId;
        perms.appName = config.manifest.name;
        perms.declared = config.manifest.permissions;
        perms.hostGrants = config.hostGrants;
        perms.dirs = appDirsFor(perms.appId);
        for (const auto& [prefix, dir] : assetMounts_.mounts()) perms.readRoots.push_back(dir);
        for (const std::string& d : util::modelDirs(settings_->getString("models.dirs")))
            perms.readRoots.push_back(d);
        appPermissions().configure(perms);
        // The powerbox: a file or folder picked in bro's own dialogs is the
        // app's to read and write.
        platform::Dialogs::setPickHook([](const std::vector<std::string>& paths) {
            appPermissions().grantPicked(paths);
        });
    }

    auto& gfx = settings_->graphics();
    auto& inp = settings_->input();
    viewportWidth_ = gfx.width;
    viewportHeight_ = gfx.height;
    frameCapIntervalMs_ = gfx.maxFps > 0.0 ? 1000.0 / gfx.maxFps : 0.0;
    inputConfig_.scrollSpeed = inp.scrollSpeed;
    inputConfig_.doubleClickThresholdMs = inp.doubleClickThresholdMs;
    inputConfig_.doubleClickDistancePx = inp.doubleClickDistancePx;

    engineNowMs_ = displayMode_ == DisplayMode::Headless ? virtualTime_
                                                         : util::currentTimeMs();

    // Physics, networking, Steam, audio and audio inference are made on
    // first use (engine.cpp: physicsWorld(), netService(), ...).
    audioStartup_.appId = config.appId;
    audioStartup_.appName = config.manifest.name.empty() ? config.appId : config.manifest.name;
    audioStartup_.realAudio = config.realAudio;
    serverStartTime_ = util::currentTimeMs();

    // Frames skip what is still building, a window's and headless's alike:
    // headless runs the frames a window shows. Its reads (capturePixels) and
    // a page's captureFrame draw what their frame skipped (perf.asyncShaders
    // overrides).
#if BRO_WITH_3D
    scene::vk::pipeline_async::setAsync(displayMode_ != DisplayMode::Server);
#endif

    if (displayMode_ == DisplayMode::Server) {
        LOG_INFO("Server mode initializing (headless server tick loop)");
    }

    // Graphics initialization.
    // - Headless: an offscreen VulkanContext + presenter (no surface, no X11).
    // - Windowed: a platform::Window and SkiaRenderer, presented through a
    //   VulkanSwapchain + VulkanPresenter.
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
#if defined(_WIN32)
    // The session's shell sees every key first (desktop_hotkeys.h): a Super
    // tap and the Win chords the system keeps reach its hotkeys, and a
    // grab's keys reach its document while another app has the foreground.
    if (desktopShell_ && displayMode_ == DisplayMode::Windowed) {
        platform::desktop::setShellKeySink([this](uint32_t evdev, bool down, bool repeat, uint32_t mods) {
            deliverShellGrabKey(evdev, down, repeat, mods);
        });
        platform::desktop::setDesktopShellKeyboard(true);
    }
#endif
    GraphicsStartup gfxTiming;
    gfxTiming.startAtMs = sinceStartMs();
    const double gfxStart = util::currentTimeMs();
    render::SkiaGpu* headlessSkiaGpu = nullptr;
    std::string gpuInitError;
    std::thread gpuInit;
    double gpuMs = -1.0, gpuReadyAt = -1.0, windowDoneAt = -1.0;
    std::atomic<bool> gpuInitDone{false};
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
        {
            gpuInit = std::thread([this, &headlessSkiaGpu, &gpuInitError, &gpuMs, &gpuReadyAt, &gpuInitDone] {
                const double t0 = util::currentTimeMs();
                struct Timed {
                    double t0;
                    double& out;
                    double& readyAt;
                    std::atomic<bool>& done;
                    ~Timed() {
                        out = util::currentTimeMs() - t0;
                        readyAt = sinceStartMs();
                        done.store(true, std::memory_order_release);
                    }
                } timed{t0, gpuMs, gpuReadyAt, gpuInitDone};
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
                    if (!headlessSkiaGpu) gpuInitError = "Headless Skia initialization failed (no Ganesh context on the device)";
                } catch (const std::exception& e) {
                    gpuInitError = std::string("Headless Vulkan initialization failed: ") + e.what();
                }
            });
        }

        try {
            // Never a Vulkan window: headless renders offscreen and presents
            // nothing, so its window needs no surface, and a Vulkan one has
            // SDL load the Vulkan loader and driver, which waits on the GPU
            // thread loading them (Linux RADV: 17 ms against 3; Windows,
            // NVIDIA's ICD: ~105 against ~28), and the page's host globals
            // wait on the window.
            platform::WindowConfig wc = primaryWindowConfig(config, gfx, platform::GraphicsBackend::Software);
            wc.hidden = true;
            window_ = platform::createWindow(wc);

            const auto& wcfg = config.graphics;
            if (wcfg.alwaysOnTop) window_->setAlwaysOnTop(true);
            if (wcfg.minWidth > 0 || wcfg.minHeight > 0)
                window_->setMinimumSize(wcfg.minWidth, wcfg.minHeight);
            if (wcfg.maxWidth > 0 || wcfg.maxHeight > 0)
                window_->setMaximumSize(wcfg.maxWidth, wcfg.maxHeight);
            if (wcfg.clickThrough) setClickThrough(true);
        } catch (const std::exception& e) {
            LOG_INFO("Headless window creation skipped: %s", e.what());
            window_.reset();
        }
        gfxTiming.windowMs = util::currentTimeMs() - gfxStart;
        windowDoneAt = sinceStartMs();
    } else if (displayMode_ == DisplayMode::Windowed) {
        // Preload the Vulkan loader / ICD and create VkInstance on a worker
        // thread while the main thread creates the window and begins compiling
        // the page's initial script.
        render::VulkanContextConfig vkCfg;
        vkCfg.headless = false;
        vulkanContext_ = std::make_unique<render::VulkanContext>(vkCfg);
        gpuInit = std::thread([this, &gpuMs]() {
            const double t0 = util::currentTimeMs();
            vulkanContext_->preloadInstance();
            gpuMs += util::currentTimeMs() - t0;
        });
        {
            window_ = platform::createWindow(primaryWindowConfig(config, gfx, platform::GraphicsBackend::Vulkan));

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
            // Everything passes through until the first frame has laid the
            // page out and given the window its input region.
            if (wcfg.clickThrough && !setClickThrough(true))
                LOG_WARN("bro.json clickThrough: this window system cannot pass input through a window");
        }
        gfxTiming.windowMs = util::currentTimeMs() - gfxStart;
        windowDoneAt = sinceStartMs();
    } else if (displayMode_ == DisplayMode::Drm) {
        initDrm(config);
    } else {
        // bro-server: no page thread to keep answering.
        removePagePump();
        renderer_ = std::make_unique<render::RasterRenderer>();
    }

    // The window is up: from here to the first frame, the longest stretch
    // the page thread goes without letting the OS in is what a window system
    // judges (util/page_pump.h; logged with "first frame").
    util::startPumpWatch();
    util::notePagePump();
    if (displayMode_ != DisplayMode::Server) bronze_host::startScriptPumpWatchdog();

    // The page's first script compiles on its own thread from here; its run
    // (initAppRealm) takes the program. Windowed, the device below comes up
    // on this thread meanwhile; headless, it is already coming up on its own.
    if (displayMode_ == DisplayMode::Headless || displayMode_ == DisplayMode::Windowed) beginPageCompile();

    if (displayMode_ == DisplayMode::Headless) {
        // The device's thread may still be loading the driver and the
        // pipeline cache: waited for pumped, as every wait on this thread is.
        util::waitPumpingUntil([&gpuInitDone](int ms) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (!gpuInitDone.load(std::memory_order_acquire)) {
                if (ms >= 0 && std::chrono::steady_clock::now() >= end) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return true;
        });
        if (gpuInit.joinable()) gpuInit.join();
        if (!gpuInitError.empty()) throw std::runtime_error(gpuInitError);
#if BRO_WITH_3D
        scene::SceneRenderer::setDefaultVulkanContext(vulkanContext_.get());
#endif
        LOG_INFO("Engine: Headless Vulkan initialized successfully");
        auto skia = std::make_unique<render::SkiaRenderer>();
        skia->setGpu(headlessSkiaGpu);
        renderer_ = std::move(skia);
    } else if (displayMode_ == DisplayMode::Windowed) {
        // The device is chosen against the window's surface (VulkanContext::
        // init), and the surface stays on this thread (a Cocoa view must be
        // made on the main thread), while VkInstance creation has already been
        // preloaded on gpuInit alongside window creation and the page compile.
        if (gpuInit.joinable()) gpuInit.join();
        const double gpuStart = util::currentTimeMs();
        {
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
            render::SkiaGpu* skiaGpu = createSkiaGpu();
            if (!skiaGpu) throw std::runtime_error("Windowed Skia initialization failed (no Ganesh context on the device)");
            auto skia = std::make_unique<render::SkiaRenderer>();
            skia->setGpu(skiaGpu);
            renderer_ = std::move(skia);
            gpuMs += util::currentTimeMs() - gpuStart;
            gpuReadyAt = sinceStartMs();
        }
    }
    if (displayMode_ == DisplayMode::Headless || displayMode_ == DisplayMode::Windowed) {
        gfxTiming.gpuMs = gpuMs;
        gfxTiming.readyAtMs = gpuReadyAt;
        // The window and the device only, not the audio and the page's host
        // globals that run between them (beginPageCompile): both headless and
        // windowed overlap GPU bringup with window creation.
        gfxTiming.totalMs = std::max(windowDoneAt, gpuReadyAt) - gfxTiming.startAtMs;
        noteGraphicsStartup(gfxTiming);
    }
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
            if (key == "maxFps" || key == "*")
                frameCapIntervalMs_ = g.maxFps > 0.0 ? 1000.0 / g.maxFps : 0.0;
        }
        if ((category == "audio" || category == "*") && audioEngine_) {
            auto& a = settings_->audio();
            float vol = a.muted ? 0.0f : a.masterVolume;
            audioEngine_->setMasterGain(vol);
        }
        if (category == "input" || category == "*") {
            auto& inpSettings = settings_->input();
            inputConfig_.scrollSpeed = inpSettings.scrollSpeed;
            inputConfig_.doubleClickThresholdMs = inpSettings.doubleClickThresholdMs;
            inputConfig_.doubleClickDistancePx = inpSettings.doubleClickDistancePx;
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
    document_->setScrollAbsorber(&layerScrollAbsorber_);
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
        importedBase = std::filesystem::path(path).parent_path().string();
        // The sheet named the file or URL: the host's access gate decides
        // whether it may be read or fetched (util/subresource_access.h).
        if (const std::string refused = util::subresourceRefusal(path); !refused.empty()) {
            LOG_WARN("@import: %s", refused.c_str());
            return std::string();
        }
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
        if (a11yBridge_) a11yBridge_->shutdown();
        a11yBridge_ = std::make_unique<a11y::AccessibilityBridge>();
        void* a11yNativeWin = nullptr;
#if defined(_WIN32)
        if (window_) a11yNativeWin = platform::desktop::hwndOf(window_.get());
#endif
        a11yBridge_->initialize(document_.get(), a11yNativeWin);
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
        const std::vector<PageScriptUnit> units = pageScriptUnits();
        if (!aotBuildDir_.empty()) {
            // `bro-headless --aot-build`: this realm is the one the units are
            // compiled against, and none of them runs.
            if (!bro::bronze_host::buildAppAot(*this, units, aotBuildDir_)) setTestFailure(true);
        } else {
            for (const auto& unit : units) {
                if (!bro::bronze_host::evalAppScript(*this, unit.code, unit.name, unit.moduleFile)) {
                    setTestFailure(true);
                }
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
        if (script.isModule || script.isLua) continue;
        std::string code = script.isInline() ? script.code : AppLoader::loadFile(script.path);
        if (!code.empty()) {
            if (!combined.empty()) combined += "\n;\n";
            combined += code;
        }
    }
    return combined;
}

// Module scripts are deferred on the web — they run after the classic ones, in
// document order — and each is its own module with its own URL: an external
// one is named for its file, so import.meta.url and its relative imports
// resolve from where it lives rather than from index.html; an inline one is
// named for the page, whose URL is its base. An external module script IS the
// module instance for its file: it is published under its path, so a later
// `import` of that file — from a headless driver script, or another module
// script — binds the instance that ran rather than running the file again. Two
// tags with one src are one module, evaluated once (HTML's module map).
std::vector<Engine::PageScriptUnit> Engine::pageScriptUnits() const {
    std::vector<PageScriptUnit> units;
    if (displayMode_ == DisplayMode::Server && !manifest_.htmlPath.empty()) return units;
    std::string classic = combinedClassicScripts();
    if (!classic.empty()) {
        std::string unitName = !manifest_.htmlPath.empty()
            ? manifest_.htmlPath
            : (!manifest_.scripts.empty() ? manifest_.scripts.front().path : appDir_);
        units.push_back({std::move(classic), unitName, false});
    }
    std::vector<std::string> moduleFiles;
    for (const auto& script : manifest_.scripts) {
        if (!script.isModule && !script.isLua) continue;
        if (!script.isInline()) {
            if (std::find(moduleFiles.begin(), moduleFiles.end(), script.path) != moduleFiles.end()) continue;
            moduleFiles.push_back(script.path);
        }
        std::string code = script.isInline() ? script.code : AppLoader::loadFile(script.path);
        if (code.empty()) continue;
        std::string unitName = script.isInline()
            ? (script.isLua ? (manifest_.htmlPath + ".lua") : manifest_.htmlPath)
            : script.path;
        units.push_back({std::move(code), std::move(unitName), !script.isInline() && !script.isLua});
    }
    return units;
}

// Only the FIRST unit compiles ahead: it is compiled against the realm as it
// stands before any of the page's scripts ran, which is the realm its compile
// at run time would see too. A later unit may import a module an earlier one
// published, so its compile has to wait for that run.
void Engine::beginPageCompile() {
    if (!appHtmlOverride_.empty()) return;
    manifest_ = AppLoader::loadApp(appDir_, &assetMounts_);
    util::setAssetPathContext(manifest_.basePath, &assetMounts_);
    if (!aotBuildDir_.empty()) return;
    const std::vector<PageScriptUnit> units = pageScriptUnits();
    if (units.empty()) return;
    bronze_host::startAppScriptCompile(*this, units[0].code, units[0].name, units[0].moduleFile);
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
} // namespace bro::engine
