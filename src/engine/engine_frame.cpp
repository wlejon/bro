#include "engine/engine.h"
#include "engine/control.h"
#include "engine/frame_trace.h"
#include "engine/engine_drm.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "engine/navmesh_subsystem.h"
#include "engine/scene_audio_sync.h"
#include "render/vulkan_presenter.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "layout/box.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#if BRO_WITH_NET
#include "net/net_service.h"
#endif
#if BRO_WITH_PHYSICS
#include "physics/physics_world.h"
#endif
#include "audio_inference/audio_inference.h"
#include "platform/desktop_platform.h"
#include "platform/event_loop.h"
#include "platform/window.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/native_window.h"
#include "render/skia_backend.h"

#include <broaudio/engine.h>
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/main_loop_wake.h"
#include "util/time.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace bro::engine {

dom::Element* Engine::liveElementOf(const WebGLEntry& entry) const {
    if (!entry.element || !entry.document) return nullptr;
    if (!dom::Document::isLiveDocument(entry.document)) return nullptr;
    auto* node = entry.document->resolveNode(entry.element, entry.elementId);
    return node ? static_cast<dom::Element*>(node) : nullptr;
}

void Engine::pruneDeadWebGLContexts() {
    webglEntries_.erase(
        std::remove_if(webglEntries_.begin(), webglEntries_.end(),
            [this](const WebGLEntry& e) { return e.element && !liveElementOf(e); }),
        webglEntries_.end());
}

void Engine::syncWebGLCanvasSizes() {
    for (auto& entry : webglEntries_) {
        if (!entry.context) continue;
        dom::Element* el = liveElementOf(entry);
        // Out of the document a canvas has no box: its drawing buffer stays
        // as it is (a resize would clear it) until it is laid out again.
        if (!el) continue;
        const dom::Node* root = el;
        while (root->parentNode()) root = root->parentNode();
        const auto* rootEl = static_cast<const dom::Element*>(root);
        if (rootEl->tagName() != "html" && rootEl->tagName() != "HTML") continue;
        auto& box = el->layoutBox();
        int elemW = static_cast<int>(box.contentRect.width);
        int elemH = static_cast<int>(box.contentRect.height);

        int cw = elemW > 0 ? elemW : 300;
        int ch = elemH > 0 ? elemH : 150;
        if (el->hasAttribute("width")) {
            try { cw = std::stoi(el->getAttribute("width")); } catch (...) {}
        }
        if (el->hasAttribute("height")) {
            try { ch = std::stoi(el->getAttribute("height")); } catch (...) {}
        }
        if (cw > 0 && ch > 0 &&
            (cw != entry.context->canvasWidth() || ch != entry.context->canvasHeight())) {
            entry.context->resize(cw, ch);
        }
    }
}

void Engine::pumpWebGLContextEvents() {
    // Indexed: a listener may create another context, growing the vector.
    for (size_t i = 0; i < webglEntries_.size(); ++i) {
        webgl::WebGL2RenderingContext* ctx = webglEntries_[i].context.get();
        dom::Element* canvas = liveElementOf(webglEntries_[i]);
        if (!ctx || !canvas) continue;
        switch (ctx->takeContextEvent()) {
            case webgl::WebGL2RenderingContext::ContextEvent::Lost: {
                // Only a cancelled lost event lets the page restore the context.
                dom::Event lost("webglcontextlost", /*bubbles=*/false, /*cancelable=*/true);
                dispatchEvent(canvas, lost);
                if (!lost.defaultPrevented()) ctx->forbidRestore();
                break;
            }
            case webgl::WebGL2RenderingContext::ContextEvent::Restored: {
                dom::Event restored("webglcontextrestored", /*bubbles=*/false, /*cancelable=*/false);
                dispatchEvent(canvas, restored);
                break;
            }
            default: break;
        }
    }
}

double Engine::serverUptime() const {
    if (serverStartTime_ <= 0.0) return 0.0;
    return (util::currentTimeMs() - serverStartTime_) / 1000.0;
}

void Engine::run() {
    if (displayMode_ == DisplayMode::Headless) {
        virtualTime_ = util::currentTimeMs();
        engineNowMs_ = virtualTime_;
        if (splashVisible_) splashStartMs_ = virtualTime_;
        return;
    }

    if (displayMode_ == DisplayMode::Server) {
        running_ = true;
        util::setMainLoopWaker(&bronze_host::wakeHostServerWait);
        if (serverTickRateExplicit_) {
            LOG_INFO("[server] Running at %.0f ticks/sec", serverTickRate_);
        } else {
            LOG_INFO("[server] Running (event-driven)");
        }

        while (running_ && !serverStopRequested_ && !bro::util::interrupted()) {
            double tickStart = util::currentTimeMs();

            double scaledTickDtMs = 0.0;
            if (lastWallTickMs_ > 0.0 && tickStart > lastWallTickMs_)
                scaledTickDtMs = (tickStart - lastWallTickMs_) * effectiveTimeScale();
            engineNowMs_ += scaledTickDtMs;
            lastWallTickMs_ = tickStart;

            if (!timePaused_) fireFrameCallbacks(scaledTickDtMs);

            if (audioEngine_) audioEngine_->update();

            for (auto& pump : framePumps_) pump();

#if BRO_WITH_PHYSICS
            if (physicsWorld_) {
                physicsWorld_->consumeStep();
                if (physicsWorld_->isIdle()) {
                    if (!physicsWorld_->hasActiveBodies()) {
                        lastPhysicsTimeMs_ = util::currentTimeMs();
                        physicsAccumMs_ = 0.0;
                    } else {
                        double stepMs = physicsWorld_->timeStep() * 1000.0;
                        double nowPhys = util::currentTimeMs();
                        if (lastPhysicsTimeMs_ == 0.0) lastPhysicsTimeMs_ = nowPhys;
                        physicsAccumMs_ += (nowPhys - lastPhysicsTimeMs_) *
                                           effectiveTimeScale();
                        lastPhysicsTimeMs_ = nowPhys;
                        if (physicsAccumMs_ >= stepMs) {
                            physicsAccumMs_ -= stepMs;
                            if (physicsAccumMs_ > stepMs * 3) physicsAccumMs_ = 0;
                            physicsWorld_->signalStep();
                        }
                    }
                }
            }
#endif

            if (!running_ || serverStopRequested_ || bro::util::interrupted()) break;

            double elapsed = util::currentTimeMs() - tickStart;
            bool fixedTick = serverTickRateExplicit_;
#if BRO_WITH_PHYSICS
            if (physicsWorld_ && physicsWorld_->hasActiveBodies()) fixedTick = true;
#endif
            if (fixedTick) {
                double tickIntervalMs = 1000.0 / serverTickRate_;
                double sleepMs = tickIntervalMs - elapsed;
                if (sleepMs > 0.5) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(static_cast<int64_t>(sleepMs * 1000.0)));
                }
            } else {
                double waitMs = 60000.0;
                if (!timePaused_) {
                    const double scale = effectiveTimeScale();
                    if (scale > 0.0) {
                        const double timerWaitMs = bronze_host::hostTimerDueInMs() / scale;
                        waitMs = std::min(waitMs, timerWaitMs);
                        if (timerWaitMs > 0.0 && bronze_host::hostFrameDueInMs() <= 0.0) {
                            const double rafWaitMs = std::max(0.0, (1000.0 / serverTickRate_) - elapsed);
                            waitMs = std::min(waitMs, rafWaitMs);
                        }
                    }
                }
                if (bronze_host::brokitHasWorkInFlight()) {
                    waitMs = std::min(waitMs, 5.0);
                }
                if (waitMs > 0.0) {
                    bronze_host::waitHostServerEvents(waitMs);
                }
            }
        }

        util::setMainLoopWaker(nullptr);
        LOG_INFO("[server] Stopped (uptime: %.1fs)", serverUptime());
        return;
    }

    if (displayMode_ == DisplayMode::Drm) {
        runDrm();
        return;
    }

    running_ = true;
    if (splashVisible_) splashStartMs_ = util::currentTimeMs();

    installWindowEventHandlers();
    windowFocused_ = window_->isFocused();

    startFramePipeline();

    eventLoop_->setModalWindowEventHook([this]() { modalFrame(); });
    startControl();
    installMainLoopWaker();

    while (running_) {
        if (bro::util::interrupted()) {
            running_ = false;
            break;
        }
        windowedFrame();
    }

    removeMainLoopWaker();
    shutdown();
}

// The event loop's callbacks into the engine: input, window state, focus.
void Engine::installWindowEventHandlers() {
    const uint32_t mainWinId = window_ ? window_->windowId() : 0;
    auto mainWin = [mainWinId](uint32_t id) { return id == mainWinId || id == 0; };
    auto host = [this](uint32_t id) -> uint64_t {
        WindowHost* h = windowHostBySdlId(id);
        return h ? h->id : 0;
    };
    eventLoop_->onQuit = [this]() { running_ = false; };
    eventLoop_->onCloseRequested = [this](uint32_t id) { handleWindowCloseRequested(id); };
    eventLoop_->onResize = [this, mainWin](uint32_t id, uint32_t w, uint32_t h) {
        if (mainWin(id)) handleResize((int)w, (int)h);
        else handleHostResized(id, (int)w, (int)h);
    };
    eventLoop_->onMouseDown = [this, mainWin, host](uint32_t id, float x, float y, uint8_t b) {
        if (mainWin(id)) handleMouseDown(x, y, (int)b);
        else hostMouseDown(host(id), x, y, (int)b);
    };
    eventLoop_->onMouseUp = [this, mainWin, host](uint32_t id, float x, float y, uint8_t b) {
        if (mainWin(id)) handleMouseUp(x, y, (int)b);
        else hostMouseUp(host(id), x, y, (int)b);
    };
    eventLoop_->onMouseMove = [this, mainWin, host](uint32_t id, float x, float y, float xr, float yr) {
        if (mainWin(id)) handleMouseMove(x, y, xr, yr);
        else hostMouseMove(host(id), x, y, xr, yr);
    };
    eventLoop_->onKeyDown = [this, mainWin, host](uint32_t id, int32_t k, int32_t s, uint16_t m, bool r) {
        if (!r) traceKeyPress();
        if (mainWin(id)) handleKeyDown(k, s, (int)m, r);
        else hostKeyDown(host(id), k, s, (int)m, r);
    };
    eventLoop_->onKeyUp = [this, mainWin, host](uint32_t id, int32_t k, int32_t s, uint16_t m, bool r) {
        if (mainWin(id)) handleKeyUp(k, s, (int)m, r);
        else hostKeyUp(host(id), k, s, (int)m, r);
    };
    eventLoop_->onTextInput = [this, mainWin, host](uint32_t id, const std::string& t) {
        if (mainWin(id)) handleTextInput(t);
        else hostTextInput(host(id), t);
    };
    eventLoop_->onTextEditing = [this, mainWin, host](uint32_t id, const std::string& t, int32_t s, int32_t l) {
        if (mainWin(id)) handleTextEditing(t, s, l);
        else hostTextEditing(host(id), t, s, l);
    };
    eventLoop_->onWheel = [this, mainWin, host](uint32_t id, float x, float y, float dx, float dy) {
        if (mainWin(id)) handleWheel(x, y, dx, dy);
        else hostWheel(host(id), x, y, dx, dy);
    };
    eventLoop_->onDropFile = [this, mainWin, host](uint32_t id, const std::vector<std::string>& p, float x, float y) {
        if (mainWin(id)) handleDropFile(p, x, y);
        else hostDropFile(host(id), p, x, y);
    };
    eventLoop_->onDropText = [this, mainWin, host](uint32_t id, const std::string& t, float x, float y) {
        if (mainWin(id)) handleDropText(t, x, y);
        else hostDropText(host(id), t, x, y);
    };
    eventLoop_->onOwnDragMotion = [this, mainWin](uint32_t id, float x, float y) {
        if (mainWin(id)) handleOwnDragMotion(x, y);
    };
    eventLoop_->onOwnDragLeave = [this, mainWin](uint32_t id) {
        if (mainWin(id)) handleOwnDragLeave();
    };
    eventLoop_->onOwnDragDrop = [this, mainWin](uint32_t id, float x, float y) {
        if (mainWin(id)) handleOwnDragDrop(x, y);
    };
    eventLoop_->onOwnDragEnd = [this](const std::string& action) { handleOwnDragEnd(action); };
    eventLoop_->onFingerDown = [this, mainWin](uint32_t wid, uint64_t id, float x, float y, float p) {
        if (mainWin(wid)) handleTouchDown(id, x, y, p);
    };
    eventLoop_->onFingerMove = [this, mainWin](uint32_t wid, uint64_t id, float x, float y, float p) {
        if (mainWin(wid)) handleTouchMove(id, x, y, p);
    };
    eventLoop_->onFingerUp = [this, mainWin](uint32_t wid, uint64_t id, float x, float y) {
        if (mainWin(wid)) handleTouchUp(id, x, y);
    };
    eventLoop_->onFingerCancel = [this, mainWin](uint32_t wid, uint64_t id, float x, float y) {
        if (mainWin(wid)) handleTouchCancel(id, x, y);
    };
    eventLoop_->onGamepadAdded = [this](uint32_t id) { handleGamepadAdded(id); };
    eventLoop_->onGamepadRemoved = [this](uint32_t id) { handleGamepadRemoved(id); };
    eventLoop_->onGamepadButton = [this](uint32_t id, int b, bool down) { handleGamepadButton(id, b, down); };
    eventLoop_->onGamepadAxis = [this](uint32_t id, int a, float v) { handleGamepadAxis(id, a, v); };
    eventLoop_->onPointerLeft = [this, mainWin](uint32_t id) {
        if (mainWin(id)) pointerInWindow_ = false;
        else if (WindowHost* h = windowHostBySdlId(id)) h->pointerIn = false;
    };
    eventLoop_->onFocusLost = [this, mainWin](uint32_t id) {
        if (mainWin(id)) {
            windowFocused_ = false;
            exitPointerLock();
            setPageVisibility(false);
            dispatchWindowFocusChange(false);
        } else {
            handleHostFocusChanged(id, false);
        }
    };
    eventLoop_->onFocusGained = [this, mainWin](uint32_t id) {
        if (mainWin(id)) {
            windowFocused_ = true;
            setPageVisibility(true);
            dispatchWindowFocusChange(true);
        } else {
            handleHostFocusChanged(id, true);
        }
    };
    eventLoop_->onMinimized = [this, mainWin](uint32_t id) {
        if (mainWin(id)) setPageVisibility(false);
        else handleHostMinimized(id, true);
    };
    eventLoop_->onMaximized = [this, mainWin](uint32_t id) {
        if (mainWin(id)) setPageVisibility(true);
        else handleHostMinimized(id, false);
    };
    eventLoop_->onRestored = [this, mainWin](uint32_t id) {
        if (mainWin(id)) setPageVisibility(true);
        else handleHostMinimized(id, false);
    };
    eventLoop_->onOccluded = [this, mainWin](uint32_t id) { if (!mainWin(id)) handleHostOccluded(id, true); };
    eventLoop_->onExposed = [this, mainWin](uint32_t id) {
        // The window system asked for the picture again: present the next
        // frame even if it is unchanged.
        if (mainWin(id)) presentedKeyValid_ = false;
        else handleHostOccluded(id, false);
    };
    eventLoop_->onSystemThemeChanged = [this]() { applyColorScheme(); };
    eventLoop_->onDisplayScaleChanged = [this, mainWin](uint32_t id) {
        if (mainWin(id)) handleDisplayScaleChanged();
        else handleHostDisplayScaleChanged(id);
    };
}

// The layout and raster workers and the handoffs between them and this thread.
void Engine::startFramePipeline() {
    rasterReady_.store(false, std::memory_order_relaxed);

    framePresenter_ = std::make_unique<FramePresenter>();
    layoutPipeline_ = std::make_unique<LayoutPipeline>();

    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);
    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);
}

// One frame of the windowed loop. Headless runs the same frame through
// runPipelineFrames (headless_pipeline.cpp), presenting to an offscreen target.
// A modal frame (modalFrame) runs from inside the poll of the frame that
// started the modal loop: it neither waits for the window system nor polls,
// and is never held idle.
void Engine::windowedFrame(bool modal) {
    // Stepped headless frames: the clock moves a fixed step per frame and
    // nothing waits on the wall clock.
    const bool stepped = headlessPipeline_ && pipelineStepMs_ > 0.0;
    {
        // Where the window system says when it wants the next frame (a
        // Wayland frame callback), wait for that here, before input is read
        // and the frame built: the frame then carries the newest input and
        // its present goes out at once. Without the wait, a vsynced present
        // holds the finished frame until the compositor's next frame, and
        // everything it shows is a refresh older than it need be.
        const double frameWaitStart = util::currentTimeMs();
        if (window_ && !headlessPipeline_ && !modal) window_->waitForFrame(kFrameWaitMaxMs);
        double frameStart = util::currentTimeMs();

        // Where the window system reports presentation (Wayland), the clock
        // advances to the vblank this frame is expected on, as under DRM:
        // frame starts wander within the refresh period, the screen does not.
        // Elsewhere the frame start is the best guess there is.
        double clockAt = frameStart;
        {
            const double last = frameTrace_->lastPresentationMs();
            const double period = frameTrace_->refreshPeriodMs();
            if (last > 0.0 && period > 0.0 && frameStart - last < 1000.0) {
                clockAt = last + period;
                while (clockAt < frameStart + 1.0) clockAt += period;
            }
        }
        double wallFrameDtMs = 0.0;
        if (stepped) {
            wallFrameDtMs = pipelineStepMs_;
        } else {
            if (lastWallTickMs_ > 0.0 && clockAt > lastWallTickMs_)
                wallFrameDtMs = clockAt - lastWallTickMs_;
            lastWallTickMs_ = std::max(lastWallTickMs_, clockAt);
        }
        // Headless keeps its virtual clock on the frames' time, so what a
        // script does after the run (advanceTime, timers) carries on from it.
        if (headlessPipeline_) virtualTime_ += wallFrameDtMs;
        const double scaledFrameDtMs = wallFrameDtMs * effectiveTimeScale();
        engineNowMs_ += scaledFrameDtMs;
        // The record spans the wait: it is this frame's pacing.
        traceFrameBegin(frameWaitStart);
        frameTrace_->current().pacingWaitMs = frameStart - frameWaitStart;

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
        FrameRecord& rec = frameTrace_->current();
        double tPhase = util::currentTimeMs();
        rec.eventsMs = tPhase - frameStart;
        control_->pump();
        {
            const double t = util::currentTimeMs();
            rec.controlMs = t - tPhase;
            tPhase = t;
        }

        pollAppWatcher(util::currentTimeMs());
        if (pendingAppReload_) {
            // The reload tears down what the raster worker reads, so it runs
            // against a quiescent worker. Waiting for it here, rather than
            // only reloading on a frame that happens to find it idle, is what
            // makes the reload land at all under an app that renders every
            // frame — the worker is back at work before this point comes
            // round again.
            framePresenter_->waitUntilIdle();
            processPendingAppReload();
        }

        if (document_ && !document_->isStructureDirty()) {
            document_->drainPendingFrees();
        }

        pumpVideoEvents();
        pumpTerminals();
        pumpRemoteViews();
        pumpWebGLContextEvents();

        if (framePresenter_->consumeIfReady()) traceRasterConsumed();

        if (!canvasScenesDetached_.empty() && framePresenter_->isRasterIdle()) canvasScenesDetached_.clear();
        {
            const double t = util::currentTimeMs();
            rec.miscMs = t - tPhase;
            tPhase = t;
        }

        if (!modal) {
            if (eventLoop_) {
                modalFramesRan_ = false;
                pollingEvents_ = true;
                eventLoop_->pollEvents();
                pollingEvents_ = false;
            }
            platform::desktop::pumpEvents();
            if (eventLoop_ && eventLoop_->shouldQuit()) {
                running_ = false;
                return;
            }
            // The poll ran a window move or resize, and frames of their own
            // through it (modalFrame): this one's clock and record are from
            // before it. What the poll delivered goes to the next frame.
            if (modalFramesRan_) {
                modalFramesRan_ = false;
                return;
            }
        }
        beginGpuFrame();
        {
            const double t = util::currentTimeMs();
            rec.inputMs = t - tPhase;
            tPhase = t;
        }

#if BRO_WITH_PHYSICS
        if (physicsWorld_) {
            physicsWorld_->consumeStep();
        }
#endif

#if BRO_WITH_3D
        pruneDetachedSceneGraphs();
#endif
        pruneDeadWebGLContexts();

#if BRO_WITH_3D
#if BRO_WITH_PHYSICS
        if (physicsWorld_ && physicsWorld_->interpolation()) {
            double stepMs = physicsWorld_->timeStep() * 1000.0;
            double pendingMs = physicsAccumMs_;
            if (lastPhysicsTimeMs_ > 0.0)
                pendingMs += (util::currentTimeMs() - lastPhysicsTimeMs_) *
                             effectiveTimeScale();
            physicsWorld_->setRenderAlpha(
                stepMs > 0.0 ? static_cast<float>(pendingMs / stepMs) : 1.0f);
        }
#endif
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
                sg.graph->syncAgents(frameDt);
                sg.graph->tickAnimations(frameDt);
            }
            SceneAudioSync::sync(frameDt);
#endif
            float wheelDt = wallFrameDtMs > 0.0
                ? static_cast<float>(wallFrameDtMs / 1000.0)
                : 1.0f / 60.0f;
            if (wheelDt > 0.1f) wheelDt = 0.1f;
            drainWheelSmoothing(wheelDt);
        }

        const double now = headlessPipeline_ ? virtualTime_ : util::currentTimeMs();
        tickSystemPanels(now);
        if (!timePaused_ && tickIframes(engineNowMs_)) uiDirty_ = true;
        if (!timePaused_ && tickWindowHosts(engineNowMs_)) uiDirty_ = true;
        if (systemDirty_) uiDirty_ = true;

        syncWebGLCanvasSizes();

        {
            const double tJs = util::currentTimeMs();
            if (!timePaused_) fireFrameCallbacks(scaledFrameDtMs);
            rec.jsMs = util::currentTimeMs() - tJs;
        }

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

        awaitTerminalEcho(frameStart);

        bool layoutIdle = layoutPipeline_->isIdle();
        bool layoutSignaled = false;
        // The last layout is in: what is under a still pointer now (a
        // replaced element, one appearing, a scroll), before this frame's.
        if (layoutIdle && hoverRecheckDue()) recheckHover();
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
            traceLayoutSignalled();
            layoutSignaled = true;
            noteHoverRecheck();
        }
        rec.tickMs = util::currentTimeMs() - tPhase;

        renderAndPresentFrame(frameStart, now, wallFrameDtMs, layoutSignaled, baseWasDirty);
        traceFrameEnd();

        // A held frame: nothing is due, so wait for work rather than run
        // the next frame (engine_idle.cpp). Outside the frame records: an
        // idle stretch is not a frame.
        if (idleHeld_) {
            idleHeld_ = false;
            if (!stepped && !modal) idleWait();
        }
    }
}

// Win32 runs its own message loop for as long as the user drags the window
// by its title bar or an edge, inside the frame's pollEvents, and calls this
// on a timer from inside it. Frames go on there: timers, rAF, audio fed from
// script, layout and paint, at the size the window has now. Only from that
// poll, and one at a time: anything else that pumps the OS's messages is not
// at a point between frames.
void Engine::modalFrame() {
    if (!pollingEvents_ || inModalFrame_ || !running_) return;
    if (window_) {
        int w = 0, h = 0;
        window_->getSize(w, h);
        // The resize events wait in SDL's queue until the drag ends.
        if (w > 0 && h > 0 && (w != viewportWidth_ || h != viewportHeight_)) handleResize(w, h);
    }
    inModalFrame_ = true;
    windowedFrame(/*modal=*/true);
    inModalFrame_ = false;
    modalFramesRan_ = true;
}

void Engine::removeModalEventWatch() {
    if (eventLoop_) eventLoop_->setModalWindowEventHook(nullptr);
}

void Engine::flushLayoutForRead(dom::Document* doc) {
    if (!doc || !textMetrics_ || !doc->isDirty() || !doc->documentElement()) return;
    if (doc->layoutIsCurrent()) return;
    if (layoutPipeline_ && !layoutPipeline_->isIdle()) return;

    float width = 0.0f, height = 0.0f;
    dom::Element* hovered = nullptr;
    if (doc == document_.get()) {
        width  = static_cast<float>(viewportWidth_);
        height = static_cast<float>(contentHeight());
        hovered = hoveredElement_.get();
    } else {
        bool found = false;
        for (auto& sys : systemDocs_) {
            if (sys.document.get() != doc) continue;
            if (!isSystemDocVisible(sys)) return;
            width  = static_cast<float>(viewportWidth_);
            height = static_cast<float>(viewportHeight_);
            found = true;
            break;
        }
        for (auto& frame : iframeDocs_) {
            if (!frame || frame->document.get() != doc) continue;
            width  = static_cast<float>(frame->boxW);
            height = static_cast<float>(frame->boxH);
            hovered = frame->hoveredElement;
            found = true;
            break;
        }
        for (auto& host : windowHosts_) {
            if (!host || host->pendingClose || host->document.get() != doc) continue;
            width  = static_cast<float>(host->boxW);
            height = static_cast<float>(host->boxH);
            hovered = host->hoveredElement;
            found = true;
            break;
        }
        if (!found || width <= 0.0f) return;
    }

    const double tForced = util::currentTimeMs();
    if (doc->isStructureDirty()) ensureReplacedElements(doc->documentElement());

    dom::Element* previousHover = hoveredElement_.get();
    layout::ElementRefAdapter::setHoveredElement(hovered);
    // A transition or animation this resolve starts starts now, on the
    // scaled clock, as one the layout thread starts does (it hands the
    // document its pass's time). The document still held the last pass's: a
    // page idle for a while (a tooltip's timer) started a 120 ms fade-in
    // hundreds of ms in the past, finished at its first tick, and left the
    // frame it was recorded in (opacity 0) on screen until the next input.
    if (doc == document_.get()) doc->setTransitionManager(&transitionManager_, engineNowMs_);
    doc->resolveStyles();
    if (doc->isLayoutDirty() || doc->isStructureDirty() || !doc->layoutRoot())
        doc->performLayout(width, height, *textMetrics_);

    doc->clearDirty();
    doc->markPaintDirty();

    if (hovered != previousHover)
        layout::ElementRefAdapter::setHoveredElement(previousHover);

    if (doc == document_.get())
        updateDocumentHeight();

    doc->noteLayoutCurrent();

    // Script-forced layout (a geometry read after a mutation), for the trace.
    if (frameTrace_) {
        FrameRecord& rec = frameTrace_->current();
        ++rec.forcedLayouts;
        rec.forcedLayoutMs += util::currentTimeMs() - tForced;
    }
}

void Engine::dispatchWindowFocusChange(bool focused) {
    if (document_ && document_->documentElement()) {
        dom::Event evt(focused ? "focus" : "blur", false, false);
        evt.setIsTrusted(true);
        dispatchEvent(document_->documentElement(), evt);
    }
    bronze_host::dispatchWindowFocus(focused);
}

} // namespace bro::engine
