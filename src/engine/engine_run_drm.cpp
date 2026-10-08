// The DRM display mode's frame loop: bro is the display server, presenting
// through KMS and reading input from libinput. One frame drains the layout
// thread's events, polls the seat / input / compositor, ticks the world,
// signals layout and renders. Input routing lives in engine_drm_input.cpp.
#include "engine/engine.h"
#include "engine/control.h"
#include "engine/frame_trace.h"
#include "engine/engine_drm.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/navmesh_subsystem.h"
#include "engine/replaced_elements.h"
#include "platform/desktop_hotkeys.h"
#include "render/vulkan_presenter.h"
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
#include "platform/drm_input.h"
#include "platform/drm_seat.h"
#include "render/kms_direct_presenter.h"
#endif
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
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
#include "util/time.h"

#include <algorithm>
#include <thread>

namespace bro::engine {

void Engine::runDrm() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    running_ = true;
    windowFocused_ = true;
    if (splashVisible_) splashStartMs_ = util::currentTimeMs();
    platform::desktop::resetHotkeyKeyState();

    rasterReady_.store(false, std::memory_order_relaxed);
    framePresenter_ = std::make_unique<FramePresenter>();
    layoutPipeline_ = std::make_unique<LayoutPipeline>();
    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);
    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);
    startControl();

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        drmFrame();
    }

    shutdown();
#endif
}

void Engine::drmFrame() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    const double frameStart = util::currentTimeMs();
    double wallFrameDtMs = 0.0;
    if (lastWallTickMs_ > 0.0 && frameStart > lastWallTickMs_) wallFrameDtMs = frameStart - lastWallTickMs_;
    lastWallTickMs_ = frameStart;
    const double scaledFrameDtMs = wallFrameDtMs * effectiveTimeScale();
    engineNowMs_ += scaledFrameDtMs;
    traceFrameBegin(frameStart);
    FrameRecord& rec = frameTrace_->current();

    drmDrainLayoutEvents();
    double t = util::currentTimeMs();
    rec.eventsMs = t - frameStart;

    // Agent control commands, while the layout thread is idle (docs/agent-control.md).
    control_->pump();
    double t1 = util::currentTimeMs();
    rec.controlMs = t1 - t;

    pollAppWatcher(util::currentTimeMs());
    if (pendingAppReload_) {
        framePresenter_->waitUntilIdle();
        processPendingAppReload();
    }
    if (document_ && !document_->isStructureDirty()) document_->drainPendingFrees();

    pumpVideoEvents();
    pumpTerminals();
    pumpWebGLContextEvents();

    if (framePresenter_->consumeIfReady()) traceRasterConsumed();
    if (!canvasScenesDetached_.empty() && framePresenter_->isRasterIdle()) canvasScenesDetached_.clear();

    t = util::currentTimeMs();
    rec.miscMs = t - t1;
    drmPollPlatform();
    beginGpuFrame();
    double t2 = util::currentTimeMs();
    rec.inputMs = t2 - t;

    const double now = drmTickWorld(scaledFrameDtMs);
    rec.tickMs = util::currentTimeMs() - t2;
    const bool baseWasDirty = document_ && document_->isDirty();
    const bool layoutSignaled = drmSignalLayout(baseWasDirty);

    renderAndPresentFrame(frameStart, now, wallFrameDtMs, layoutSignaled, baseWasDirty);
    pollScreenCaptureTriggers();

    t = util::currentTimeMs();
    if (vulkanPresenter_ && vulkanPresenter_->kmsDirectPresenter())
        vulkanPresenter_->kmsDirectPresenter()->handlePageFlipEvent(10);
    rec.pacingWaitMs += util::currentTimeMs() - t;
    traceFrameEnd();
#endif
}

// Transition / animation events the layout thread produced last pass.
void Engine::drmDrainLayoutEvents() {
    if (!layoutPipeline_->waitForIdle()) return;
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

// Seat (VT switches), libinput, then the compositor's client events.
void Engine::drmPollPlatform() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    if (!drmCtx_) return;
    if (drmCtx_->seat) drmCtx_->seat->pollEvents();
    if (drmCtx_->input)
        drmCtx_->input->pollEvents([this](const platform::DrmInputEvent& ev) {
            ++frameTrace_->current().inputEvents;
            dispatchDrmInput(ev);
        });
    // Client events, then the shell's window frames moved with them.
    if (pollShellCompositor()) uiDirty_ = true;
#endif
}

// Physics, scenes, panels, iframes, frame callbacks: everything that advances
// with the clock before layout is signalled.
double Engine::drmTickWorld(double scaledFrameDtMs) {
#if BRO_WITH_PHYSICS
    if (physicsWorld_) physicsWorld_->consumeStep();
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
    webglEntries_.erase(std::remove_if(webglEntries_.begin(), webglEntries_.end(),
                                       [&](auto& e) { return isDetached(e.element); }),
                        webglEntries_.end());
#if BRO_WITH_3D
    for (auto& sg : sceneGraphs_) sg.graph->syncPhysics();
#endif

    {
        double nowMs = engineNowMs_;
        float frameDt = (lastFrameTimeMs_ > 0.0) ? static_cast<float>((nowMs - lastFrameTimeMs_) / 1000.0)
                                                 : 1.0f / 60.0f;
        frameDt = std::clamp(frameDt, 0.0f, 0.1f);
        lastFrameTimeMs_ = nowMs;
        bro::engine::pumpNavMeshObstacles(frameDt);
#if BRO_WITH_3D
        for (auto& sg : sceneGraphs_) sg.graph->tickAnimations(frameDt);
#endif
    }

    const double now = util::currentTimeMs();
    tickSystemPanels(now);
    if (!timePaused_ && tickIframes(engineNowMs_)) uiDirty_ = true;
    if (!timePaused_ && tickWindowHosts(engineNowMs_)) uiDirty_ = true;
    if (systemDirty_) uiDirty_ = true;

    syncWebGLCanvasSizes();
    const double tJs = util::currentTimeMs();
    if (!timePaused_) fireFrameCallbacks(scaledFrameDtMs);
    for (auto& pump : framePumps_) pump();
    frameTrace_->current().jsMs = util::currentTimeMs() - tJs;

#if BRO_WITH_3D
    if (auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get())) {
        for (auto& sg : sceneGraphs_)
            if (sg.graph) sg.graph->materializeHtmlNodes(skia);
    }
    for (auto& sg : sceneGraphs_) {
        if (sg.element) {
            auto& box = sg.element->layoutBox();
            int ew = static_cast<int>(box.contentRect.width);
            int eh = static_cast<int>(box.contentRect.height);
            if (ew > 0 && eh > 0 && (ew != sg.graph->canvasWidth() || eh != sg.graph->canvasHeight()))
                sg.graph->setCanvasSize(ew, eh);
        }
        sg.graph->setDeviceScale(deviceScale_.render);
        sg.graph->render();
    }
#endif

#if BRO_WITH_PHYSICS
    if (physicsWorld_ && physicsWorld_->isIdle()) {
        if (!physicsWorld_->hasActiveBodies()) {
            lastPhysicsTimeMs_ = util::currentTimeMs();
            physicsAccumMs_ = 0.0;
        } else {
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
    }
#endif
    return now;
}

// Hands the layout thread a pass when the document (or an animation, or a
// scene's HTML nodes) needs one and both pipelines are idle.
bool Engine::drmSignalLayout(bool baseWasDirty) {
    const bool animActive = layoutPipeline_->animationsActive();
    bool sceneHtmlDirty = false;
#if BRO_WITH_3D
    for (auto& sg : sceneGraphs_) {
        if (sg.graph && sg.graph->hasPendingHtmlWork()) {
            sceneHtmlDirty = true;
            break;
        }
    }
#endif
    if (!layoutPipeline_->isIdle() || !framePresenter_->isRasterIdle() || !document_) return false;
    if (!(baseWasDirty || animActive || sceneHtmlDirty || !hasRenderedOnce_)) return false;

    if (document_->isStructureDirty()) {
        ensureReplacedElements(document_->documentElement());
        iframeSyncNeeded_ = true;
    }
    LayoutPipeline::Snapshot ls;
    ls.vpWidth = viewportWidth_;
    ls.vpHeight = viewportHeight_;
    ls.insetTop = contentTop();
    ls.insetRight = contentRight();
    ls.insetBottom = contentBottom();
    ls.animationsActive = animActive;
    ls.hoveredElement = hoveredElement_.get();
    ls.timeMs = engineNowMs_;
    layoutPipeline_->signalLayout(ls);
    traceLayoutSignalled();
    return true;
}

}  // namespace bro::engine
