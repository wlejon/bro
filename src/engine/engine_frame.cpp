#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/overflow.h"
#include "engine/replaced_elements.h"
#include "engine/navmesh_subsystem.h"
#include "engine/scene_audio_sync.h"

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
#include "platform/event_loop.h"
#include "platform/sdl_window.h"
#include "render/gl_context.h"
#include "render/skia_backend.h"

#include <broaudio/engine.h>
#if BRO_WITH_3D
#include "scene/scene_graph.h"
#endif
#include "webgl/webgl2_context.h"
#include "util/interrupt.h"
#include "util/log.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace bro::engine {

void Engine::syncWebGLCanvasSizes() {
    for (auto& entry : webglEntries_) {
        if (!entry.element || !entry.context) continue;
        auto& box = entry.element->layoutBox();
        int elemW = static_cast<int>(box.contentRect.width);
        int elemH = static_cast<int>(box.contentRect.height);

        int cw = elemW > 0 ? elemW : 300;
        int ch = elemH > 0 ? elemH : 150;
        if (entry.element->hasAttribute("width")) {
            try { cw = std::stoi(entry.element->getAttribute("width")); } catch (...) {}
        }
        if (entry.element->hasAttribute("height")) {
            try { ch = std::stoi(entry.element->getAttribute("height")); } catch (...) {}
        }
        if (cw > 0 && ch > 0 &&
            (cw != entry.context->canvasWidth() || ch != entry.context->canvasHeight())) {
            entry.context->resize(cw, ch);
        }
    }
}

double Engine::serverUptime() const {
    if (serverStartTime_ <= 0.0) return 0.0;
    return (util::currentTimeMs() - serverStartTime_) / 1000.0;
}

static bool modalEventWatcher(void* userdata, SDL_Event* event)
{
    if (event->type >= SDL_EVENT_WINDOW_FIRST &&
        event->type <= SDL_EVENT_WINDOW_LAST) {
        static_cast<Engine*>(userdata)->tickTimersOnly();
    }
    return true;
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
        LOG_INFO("[server] Running at %.0f ticks/sec", serverTickRate_);

        while (running_ && !serverStopRequested_ && !bro::util::interrupted()) {
            double tickStart = util::currentTimeMs();
            double tickIntervalMs = 1000.0 / serverTickRate_;

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
#endif

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

    const uint32_t mainWinId = window_->windowId();
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
    eventLoop_->onFocusLost = [this, mainWin](uint32_t id) {
        if (mainWin(id)) { windowFocused_ = false; exitPointerLock(); setPageVisibility(false); }
        else handleHostFocusChanged(id, false);
    };
    eventLoop_->onFocusGained = [this, mainWin](uint32_t id) {
        if (mainWin(id)) { windowFocused_ = true; setPageVisibility(true); }
        else handleHostFocusChanged(id, true);
    };
    eventLoop_->onMinimized = [this, mainWin](uint32_t id) {
        if (mainWin(id)) setPageVisibility(false);
        else handleHostMinimized(id, true);
    };
    eventLoop_->onMaximized = [this, mainWin](uint32_t id) { if (mainWin(id)) setPageVisibility(true); };
    eventLoop_->onRestored = [this, mainWin](uint32_t id) {
        if (mainWin(id)) setPageVisibility(true);
        else handleHostMinimized(id, false);
    };
    eventLoop_->onOccluded = [this, mainWin](uint32_t id) { if (!mainWin(id)) handleHostOccluded(id, true); };
    eventLoop_->onExposed = [this, mainWin](uint32_t id) { if (!mainWin(id)) handleHostOccluded(id, false); };
    eventLoop_->onSystemThemeChanged = [this]() { applyColorScheme(); };
    eventLoop_->onDisplayScaleChanged = [this, mainWin](uint32_t id) { if (mainWin(id)) handleDisplayScaleChanged(); };

    windowFocused_ =
        (SDL_GetWindowFlags(window_->getSDLWindow()) & SDL_WINDOW_INPUT_FOCUS) != 0;

    {
        auto ctx = window_->createSharedContext();
        if (ctx) {
            canvasRasterThread_ = std::make_unique<canvas::CanvasRasterThread>();
            canvasRasterThread_->start(ctx, window_->getSDLWindow());
        }
    }
    if (canvasRasterThread_ && canvasRasterThread_->started()) {
        for (auto& cs : canvasScenes_) {
            if (cs && !cs->isThreaded()) cs->bindRasterThread(canvasRasterThread_.get());
        }
    }

    rasterGLContext_ = window_->createSharedContext();
    if (!rasterGLContext_) {
        LOG_ERROR("Failed to create shared GL context for raster thread");
        return;
    }
    rasterReady_.store(false, std::memory_order_relaxed);

    framePresenter_ = std::make_unique<FramePresenter>();
    layoutPipeline_ = std::make_unique<LayoutPipeline>();

    layoutThread_ = std::thread(&Engine::layoutThreadFunc, this);
    rasterThread_ = std::thread(&Engine::rasterThreadFunc, this);
    rasterReady_.wait(false, std::memory_order_acquire);

    SDL_AddEventWatch(modalEventWatcher, this);

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
            if (document_ && document_->documentElement()) {
                auto& box = document_->documentElement()->layoutBox();
                documentHeight_ = box.marginBox().height;
            }
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

        if (pendingAppReload_ && framePresenter_->isRasterIdle()) {
            processPendingAppReload();
        }

        if (document_ && !document_->isStructureDirty()) {
            document_->drainPendingFrees();
        }

        pumpVideoEvents();

        framePresenter_->consumeIfReady();

        if (!canvasScenesDetached_.empty() && framePresenter_->isRasterIdle()) {
            for (auto& cs : canvasScenesDetached_) {
                if (cs->isThreaded() && canvasRasterThread_)
                    canvasRasterThread_->releaseScene(cs.get());
            }
            canvasScenesDetached_.clear();
        }

        eventLoop_->pollEvents();
        if (eventLoop_->shouldQuit()) {
            running_ = false;
            break;
        }

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

        double now = util::currentTimeMs();
        tickSystemPanels(now);
        if (!timePaused_ && tickIframes(engineNowMs_)) uiDirty_ = true;
        if (!timePaused_ && tickWindowHosts(engineNowMs_)) uiDirty_ = true;
        if (systemDirty_) uiDirty_ = true;

        syncWebGLCanvasSizes();
        webgl::WebGL2RenderingContext::invalidateCurrent();
        if (!webglEntries_.empty()) webglEntries_[0].context->bindCanvasFBO();

        if (!timePaused_) fireFrameCallbacks(scaledFrameDtMs);

        for (auto& pump : framePumps_) pump();

        webgl::WebGL2RenderingContext::endAppGL();

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
    }

    shutdown();
}

void Engine::removeModalEventWatch() {
    SDL_RemoveEventWatch(modalEventWatcher, this);
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

    if (doc->isStructureDirty()) ensureReplacedElements(doc->documentElement());

    dom::Element* previousHover = hoveredElement_.get();
    layout::ElementRefAdapter::setHoveredElement(hovered);
    doc->resolveStyles();
    if (doc->isLayoutDirty() || doc->isStructureDirty() || !doc->layoutRoot())
        doc->performLayout(width, height, *textMetrics_);

    doc->clearDirty();
    doc->markPaintDirty();

    if (hovered != previousHover)
        layout::ElementRefAdapter::setHoveredElement(previousHover);

    if (doc == document_.get())
        documentHeight_ = doc->documentElement()->layoutBox().marginBox().height;

    doc->noteLayoutCurrent();
}

} // namespace bro::engine
