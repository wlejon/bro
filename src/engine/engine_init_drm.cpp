#include "engine/engine.h"
#include "engine/engine_drm.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/navmesh_subsystem.h"
#include "engine/replaced_elements.h"
#include "platform/drm_seat.h"
#include "platform/drm_input.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/box.h"
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__linux__) && BRO_WITH_DMABUF
#include <brodmabuf/gbm.h>
#endif

namespace bro::engine {

DrmPlatformContext::DrmPlatformContext() = default;
DrmPlatformContext::~DrmPlatformContext() = default;

void Engine::initDrm(const EngineConfig& config) {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    drmCtx_ = std::make_unique<DrmPlatformContext>();
    drmCtx_->seat = std::make_unique<platform::DrmSeatPlatform>();
    if (!drmCtx_->seat->initSeat("seat0")) {
        LOG_WARN("Engine: could not initialize seat0, falling back to direct DRM device access");
    }

    std::string cardNode = brodmabuf::find_card_node();
    int cardFd = -1;
    if (drmCtx_->seat->isSeatActive() && !cardNode.empty()) {
        cardFd = drmCtx_->seat->openDevice(cardNode);
    } else if (!cardNode.empty()) {
        cardFd = ::open(cardNode.c_str(), O_RDWR | O_CLOEXEC);
    }
    if (cardFd < 0) {
        throw std::runtime_error("DRM initialization failed: cannot open DRM card node " + cardNode);
    }

    render::VulkanContextConfig vkCfg;
    vkCfg.headless = true;
    vulkanContext_ = std::make_unique<render::VulkanContext>(vkCfg);
    if (!vulkanContext_->init()) {
        throw std::runtime_error("DRM VulkanContext initialization failed");
    }

    vulkanPresenter_ = std::make_unique<render::VulkanPresenter>(*vulkanContext_);
    if (!vulkanPresenter_->init()) {
        throw std::runtime_error("DRM VulkanPresenter initialization failed");
    }

    if (!vulkanPresenter_->initKms(cardFd)) {
        LOG_WARN("Engine: VulkanPresenter::initKms failed on card %s", cardNode.c_str());
    }

    viewportWidth_ = vulkanPresenter_->width() ? vulkanPresenter_->width() : static_cast<uint32_t>(config.graphics.width);
    viewportHeight_ = vulkanPresenter_->height() ? vulkanPresenter_->height() : static_cast<uint32_t>(config.graphics.height);

    drmCtx_->input = std::make_unique<platform::DrmInputPlatform>();
    drmCtx_->input->init(*drmCtx_->seat, "seat0", viewportWidth_, viewportHeight_);

    // Hook VT switch callbacks
    drmCtx_->seat->setActiveChangeCallback([this](bool active) {
        if (!active) {
            LOG_INFO("Engine: VT switched away, pausing KMS presentation");
            if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter()) {
                vulkanPresenter_->kmsDirectPresenter()->pause();
            }
        } else {
            LOG_INFO("Engine: VT switched back, restoring KMS modeset");
            if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter()) {
                vulkanPresenter_->kmsDirectPresenter()->restoreModeset();
            }
        }
    });

    auto skia = std::make_unique<render::SkiaRenderer>();
    if (vulkanPresenter_) skia->setGpu(createSkiaGpu());
    renderer_ = std::move(skia);

    LOG_INFO("Engine: DRM display mode initialized (%ux%u)", viewportWidth_, viewportHeight_);
#else
    (void)config;
    throw std::runtime_error("DRM display mode is only supported on Linux with seat and dmabuf enabled");
#endif
}

void Engine::runDrm() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    running_ = true;
    windowFocused_ = true;
    if (splashVisible_) splashStartMs_ = util::currentTimeMs();

    auto dispatchInput = [this](const platform::DrmInputEvent& ev) {
        switch (ev.type) {
            case platform::DrmInputEvent::Type::KeyDown:
                handleKeyDown(ev.keycode, ev.scancode, ev.modifiers, ev.repeat);
                break;
            case platform::DrmInputEvent::Type::KeyUp:
                handleKeyUp(ev.keycode, ev.scancode, ev.modifiers, ev.repeat);
                break;
            case platform::DrmInputEvent::Type::MouseMove:
                handleMouseMove(ev.x, ev.y, ev.dx, ev.dy);
                break;
            case platform::DrmInputEvent::Type::MouseDown:
                handleMouseDown(ev.x, ev.y, ev.button);
                break;
            case platform::DrmInputEvent::Type::MouseUp:
                handleMouseUp(ev.x, ev.y, ev.button);
                break;
            case platform::DrmInputEvent::Type::MouseWheel:
                handleWheel(ev.x, ev.y, ev.wheelDx, ev.wheelDy);
                break;
            case platform::DrmInputEvent::Type::TouchDown:
                handleTouchDown(ev.touchId, ev.x, ev.y, ev.touchPressure);
                break;
            case platform::DrmInputEvent::Type::TouchMove:
                handleTouchMove(ev.touchId, ev.x, ev.y, ev.touchPressure);
                break;
            case platform::DrmInputEvent::Type::TouchUp:
                handleTouchUp(ev.touchId, ev.x, ev.y);
                break;
            default:
                break;
        }
    };

    rasterReady_.store(false, std::memory_order_relaxed);

    framePresenter_ = std::make_unique<FramePresenter>();
    layoutPipeline_ = std::make_unique<LayoutPipeline>();

    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);
    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        double frameStart = util::currentTimeMs();

        double wallFrameDtMs = 0.0;
        if (lastWallTickMs_ > 0.0 && frameStart > lastWallTickMs_)
            wallFrameDtMs = frameStart - lastWallTickMs_;
        lastWallTickMs_ = frameStart;
        const double scaledFrameDtMs = wallFrameDtMs * effectiveTimeScale();
        engineNowMs_ += scaledFrameDtMs;

        if (layoutPipeline_->waitForIdle()) {
            updateDocumentHeight();
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

        pollAppWatcher(util::currentTimeMs());
        if (pendingAppReload_) {
            framePresenter_->waitUntilIdle();
            processPendingAppReload();
        }

        if (document_ && !document_->isStructureDirty()) {
            document_->drainPendingFrees();
        }

        pumpVideoEvents();
        pumpTerminals();
        pumpWebGLContextEvents();

        framePresenter_->consumeIfReady();

        if (!canvasScenesDetached_.empty() && framePresenter_->isRasterIdle()) canvasScenesDetached_.clear();

        if (drmCtx_) {
            if (drmCtx_->seat) drmCtx_->seat->pollEvents();
            if (drmCtx_->input) drmCtx_->input->pollEvents(dispatchInput);
        }

        beginGpuFrame();

#if BRO_WITH_PHYSICS
        if (physicsWorld_) {
            physicsWorld_->consumeStep();
        }
#endif

        auto isDetached = [](dom::Element* el) {
            if (!el) return false;
            auto* n = el;
            while (n->parentNode()) n = static_cast<dom::Element*>(n->parentNode());
            return n->tagName() != "html" && n->tagName() != "HTML";
        };
#if BRO_WITH_3D
        pruneDetachedSceneGraphs();
#endif
        webglEntries_.erase(
            std::remove_if(webglEntries_.begin(), webglEntries_.end(),
                [&](auto& e) { return isDetached(e.element); }),
            webglEntries_.end());

#if BRO_WITH_3D
        for (auto& sg : sceneGraphs_) sg.graph->syncPhysics();
#endif

        {
            double nowMs = engineNowMs_;
            float frameDt = (lastFrameTimeMs_ > 0.0)
                ? static_cast<float>((nowMs - lastFrameTimeMs_) / 1000.0)
                : 1.0f / 60.0f;
            if (frameDt < 0.0f) frameDt = 0.0f;
            if (frameDt > 0.1f) frameDt = 0.1f;
            lastFrameTimeMs_ = nowMs;
            bro::engine::pumpNavMeshObstacles(frameDt);
#if BRO_WITH_3D
            for (auto& sg : sceneGraphs_) {
                sg.graph->tickAnimations(frameDt);
            }
#endif
        }

        double now = util::currentTimeMs();
        tickSystemPanels(now);
        if (!timePaused_ && tickIframes(engineNowMs_)) uiDirty_ = true;
        if (!timePaused_ && tickWindowHosts(engineNowMs_)) uiDirty_ = true;
        if (systemDirty_) uiDirty_ = true;

        syncWebGLCanvasSizes();

        if (!timePaused_) fireFrameCallbacks(scaledFrameDtMs);

        for (auto& pump : framePumps_) pump();

#if BRO_WITH_3D
        if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
            for (auto& sg : sceneGraphs_) {
                if (sg.graph) sg.graph->materializeHtmlNodes(skia);
            }
        }

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
            sg.graph->setDeviceScale(deviceScale_.render);
            sg.graph->render();
        }
#endif

#if BRO_WITH_PHYSICS
        if (physicsWorld_ && physicsWorld_->isIdle()) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            double nowPhys = util::currentTimeMs();
            if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
            physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_) * effectiveTimeScale();
            lastPhysicsTimeMs_ = nowPhys;
            if (physicsAccumMs_ >= stepMs) {
                physicsAccumMs_ -= stepMs;
                if (physicsAccumMs_ > stepMs * 3) physicsAccumMs_ = 0;
                physicsWorld_->signalStep();
            }
        }
#endif

        bool layoutIdle = layoutPipeline_->isIdle();
        bool layoutSignaled = false;
        bool animActive = layoutPipeline_->animationsActive();

        bool sceneHtmlDirty = false;
#if BRO_WITH_3D
        for (auto& sg : sceneGraphs_) {
            if (sg.graph && sg.graph->hasPendingHtmlWork()) { sceneHtmlDirty = true; break; }
        }
#endif

        bool baseWasDirty = document_ && document_->isDirty();

        if (layoutIdle && framePresenter_->isRasterIdle() && document_ &&
            (baseWasDirty || animActive || sceneHtmlDirty || !hasRenderedOnce_)) {
            if (document_->isStructureDirty()) {
                ensureReplacedElements(document_->documentElement());
                iframeSyncNeeded_ = true;
            }
            LayoutPipeline::Snapshot ls;
            ls.vpWidth          = viewportWidth_;
            ls.vpHeight         = viewportHeight_;
            ls.insetTop         = contentTop();
            ls.insetRight       = contentRight();
            ls.insetBottom      = contentBottom();
            ls.animationsActive = animActive;
            ls.hoveredElement   = hoveredElement_.get();
            ls.timeMs           = engineNowMs_;
            layoutPipeline_->signalLayout(ls);
            layoutSignaled = true;
        }

        renderAndPresentFrame(frameStart, now, wallFrameDtMs, layoutSignaled, baseWasDirty);

        if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter()) {
            vulkanPresenter_->kmsDirectPresenter()->handlePageFlipEvent(10);
        }
    }

    shutdown();
#endif
}

} // namespace bro::engine
