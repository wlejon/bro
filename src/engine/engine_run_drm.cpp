// The DRM display mode's frame loop: bro is the display server, presenting
// through KMS and reading input from libinput. One frame drains the layout
// thread's events, polls the seat / input / compositor, ticks the world,
// signals layout and renders. Input routing lives in engine_drm_input.cpp.
#include "engine/engine.h"
#include "engine/app_runtime.h"
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

#if defined(__linux__)
#include <cerrno>
#include <ctime>
#include <poll.h>
#endif

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

    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    if (kms) {
        kms->setFlipListener([this](const render::KmsDirectPresenter::FlipInfo& flip) {
            drmFlipLanded(flip.vblankMs, flip.sequence);
        });
    }

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        drmFrame();
    }

    if (kms) {
        kms->waitForFlip(100);
        kms->setFlipListener(nullptr);
    }
#if BRO_WITH_COMPOSITOR
    if (drmCtx_ && drmCtx_->compositor) {
        drmCtx_->compositor->returnClientLayers(drmCtx_->onScreenLeases);
        drmCtx_->compositor->releaseClientLayers(drmCtx_->flipLeases, nullptr);
        drmCtx_->onScreenLeases.clear();
        drmCtx_->flipLeases.clear();
    }
#endif

    shutdown();
#endif
}

void Engine::drmFrame() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    const double frameStart = util::currentTimeMs();
    // The clock advances to the vblank this frame will be shown at, not to
    // when the frame happened to start: frame starts move about within the
    // refresh period (pacing, input handlers), and an animation sampled at
    // the start time would step unevenly on a screen that steps evenly.
    const double clockAt = drmPresentTargetMs(frameStart);
    double wallFrameDtMs = 0.0;
    if (lastWallTickMs_ > 0.0 && clockAt > lastWallTickMs_) wallFrameDtMs = clockAt - lastWallTickMs_;
    lastWallTickMs_ = std::max(lastWallTickMs_, clockAt);
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

    pumpAppInstances();
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

    drmPaceNextFrame(frameStart);
    traceFrameEnd();
#endif
}

// The vblank a frame starting at `frameStart` will flip on: the first one
// after the last flip that is still ahead of now. Without a flip yet (or off
// KMS), the start time itself.
double Engine::drmPresentTargetMs(double frameStart) const {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    if (!kms) return frameStart;
    const double period = kms->refreshPeriodMs();
    const double last = kms->lastFlip().vblankMs;
    if (period <= 0.0 || last <= 0.0 || frameStart - last > 1000.0) return frameStart;
    // A flip still in flight takes the next vblank.
    double target = last + period * (kms->flipPending() ? 2.0 : 1.0);
    while (target < frameStart + 1.0) target += period;
    return target;
#else
    return frameStart;
#endif
}

// When the next frame starts. A composited present returns once its flip is
// committed; the flip lands at vblank V, and the next frame's commit has to be
// in before V + one refresh period. Starting it at once would leave the most
// slack but sample input and the animation clock most of a period before they
// reach the screen; starting it late leaves the frame only what is left. So
// the next frame starts as late as recent frames' work allows: a decaying peak
// of the work before the commit, with headroom, decides how early.
//
// Input and agent commands are handled as they arrive, flip wait included,
// not at the next frame's start: a key for a client window reaches it at
// once, and its answer is in time for the next frame. Input that changed the
// shell document starts the next frame at once instead, so its handlers get
// the whole period to make the next vblank rather than the budget.
void Engine::drmPaceNextFrame(double frameStart) {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    if (!kms) return;
    FrameRecord& rec = frameTrace_->current();
    const double t0 = util::currentTimeMs();

    struct pollfd fds[3];
    nfds_t nInput = 0;
    if (drmCtx_ && drmCtx_->input && drmCtx_->input->pollFd() >= 0) fds[nInput++] = {drmCtx_->input->pollFd(), POLLIN, 0};
    if (control_ && control_->pollFd() >= 0) fds[nInput++] = {control_->pollFd(), POLLIN, 0};
    const int drmFd = kms->pollFd();
    int dispatches = 0;
    // Waits until `untilMs` (with `forFlip`, or until the flip lands),
    // handling input as it comes; false when it changed the shell.
    auto waitHandlingInput = [&](double untilMs, bool forFlip) -> bool {
        for (;;) {
            if (forFlip && !kms->flipPending()) return true;
            const double waitMs = untilMs - util::currentTimeMs();
            if (waitMs <= 0.25) return true;
            nfds_t n = nInput;
            if (forFlip && drmFd >= 0) fds[n++] = {drmFd, POLLIN, 0};
            struct timespec ts;
            ts.tv_sec = static_cast<time_t>(waitMs / 1000.0);
            ts.tv_nsec = static_cast<long>((waitMs - ts.tv_sec * 1000.0) * 1e6);
            const int rc = ::ppoll(fds, n, &ts, nullptr);
            if (rc < 0 && errno != EINTR) return true;
            if (rc <= 0) continue;
            if (n > nInput && (fds[nInput].revents & POLLIN)) kms->handlePageFlipEvent(0);
            bool input = false;
            for (nfds_t i = 0; i < nInput; ++i) input = input || (fds[i].revents & (POLLIN | POLLERR | POLLHUP));
            // (A source that stays readable however often it is read ends
            // the wait rather than spinning it.)
            if (input && (++dispatches > 64 || drmDispatchBetweenFrames())) return false;
        }
    };

    // This frame's flip.
    const bool committed = drmFlipFrame_ == frameNumber_;
    if (!waitHandlingInput(t0 + 50.0, true)) {
        rec.pacingWaitMs += util::currentTimeMs() - t0;
        return;
    }
#if BRO_WITH_COMPOSITOR
    // Leases held for a flip that will not land (a VT switch dropped it).
    if (!kms->flipPending() && drmCtx_ && drmCtx_->compositor && !drmCtx_->flipLeases.empty()) {
        drmCtx_->compositor->releaseClientLayers(drmCtx_->flipLeases, nullptr);
        drmCtx_->flipLeases.clear();
    }
#endif
    const double period = kms->refreshPeriodMs();
    const double lastVblank = kms->lastFlip().vblankMs;
    const bool held = heldFrame_ == frameNumber_;
    if (kms->flipPending() || period <= 0.0 || (!committed && (!held || lastVblank <= 0.0))) {
        // Nothing went to the screen (a VT switch, a failed commit): keep the
        // loop from spinning.
        waitHandlingInput(util::currentTimeMs() + 10.0, false);
        rec.pacingWaitMs += util::currentTimeMs() - t0;
        return;
    }
    // Never less than ~10 ms at 60 Hz: an input frame's handlers are not in
    // the peak until they have run, and a clicked animation's first frame
    // that misses its vblank shows as a hitch.
    if (committed) {
        const double work = std::max(0.0, t0 - frameStart - rec.flipWaitMs - rec.pacingWaitMs);
        drmWorkPeakMs_ = std::max(work, drmWorkPeakMs_ * 0.97);
    }
    const double budget = std::clamp(drmWorkPeakMs_ * 1.5 + 3.0, period * 0.6, period);
    // A held frame (nothing changed, nothing committed) keeps the beat: the
    // next frame starts where it would have after a flip at the next vblank,
    // and the screen goes on showing what it shows.
    double next = lastVblank + period - budget;
    if (!committed) {
        const double now = util::currentTimeMs();
        if (next < now + 1.0) next += std::ceil((now + 1.0 - next) / period) * period;
    }
    waitHandlingInput(next, false);
    rec.pacingWaitMs += util::currentTimeMs() - t0;
#else
    (void)frameStart;
#endif
}

bool Engine::drmDispatchBetweenFrames() {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    drmDrainLayoutEvents();  // the layout pass this frame signalled is done
    const bool wasDirty = document_ && document_->isDirty();
    control_->pump();
    drmPollPlatform();
    return !running_ || (!wasDirty && document_ && document_->isDirty());
#else
    return true;
#endif
}

void Engine::drmFlipLanded(double vblankMs, uint32_t sequence) {
#if defined(__linux__) && BRO_WITH_SEAT && BRO_WITH_DMABUF
    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    const double period = kms ? kms->refreshPeriodMs() : 0.0;
    frameTrace_->notePresentation(drmFlipFrame_, vblankMs, sequence, period);
#if BRO_WITH_COMPOSITOR
    if (drmCtx_ && drmCtx_->compositor) {
        auto* comp = drmCtx_->compositor.get();
        // Whatever was scanned out directly has left the screen now — unless
        // the flip moved only the cursor, over it.
        const bool cursorOnly = kms && kms->cursorOnlyFlip();
        if (!drmCtx_->onScreenLeases.empty() && !cursorOnly) {
            comp->returnClientLayers(drmCtx_->onScreenLeases);
            drmCtx_->onScreenLeases.clear();
        }
        if (!drmCtx_->flipLeases.empty()) {
            compositor::WaylandCompositor::FramePresentation shown;
            shown.timestampNs = static_cast<int64_t>(vblankMs * 1e6);
            shown.sequence = sequence;
            shown.refreshNs = static_cast<uint32_t>(period * 1e6);
            comp->notifyClientLayersShown(drmCtx_->flipLeases, vblankMs > 0.0 ? &shown : nullptr);
            if (drmCtx_->flipIsDirect) drmCtx_->onScreenLeases.swap(drmCtx_->flipLeases);
            else comp->returnClientLayers(drmCtx_->flipLeases);
            drmCtx_->flipLeases.clear();
        }
    }
#endif
    if (drmCtx_) drmCtx_->flipIsDirect = false;
#else
    (void)vblankMs;
    (void)sequence;
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
