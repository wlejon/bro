#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/overflow.h"
#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "webgl/webgl2_context.h"
#include "platform/sdl_window.h"
#include "util/time.h"
#if BRO_WITH_BRONZE
#include "bronze_host/host_window_open.h"
#endif

#include <glad/gl.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

namespace bro::engine {

FramePresenter::Snapshot Engine::buildRasterSnapshot() const {
    FramePresenter::Snapshot s;
    s.vpWidth     = viewportWidth_;
    s.vpHeight    = viewportHeight_;
    s.insetTop    = contentTop();
    s.insetRight  = contentRight();
    s.insetBottom = contentBottom();
    s.scrollY     = scrollY_;
    return s;
}

void Engine::renderAndPresentFrame(double frameStart, double now, double wallFrameDtMs,
                                   bool layoutSignaled, bool baseWasDirty) {
    double tRaster = util::currentTimeMs();

    double layoutWaitMs = 0.0;
    if (layoutSignaled) {
        double tWait = util::currentTimeMs();
        bool layoutClaimed = layoutPipeline_->waitClaimDone();
        layoutWaitMs = util::currentTimeMs() - tWait;
        if (layoutClaimed) {
            if (document_ && document_->documentElement()) {
                auto& box = document_->documentElement()->layoutBox();
                documentHeight_ = box.marginBox().height;
            }

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

            if (document_ && document_->documentElement()) {
                std::vector<dom::Element*> reclamped;
                if (clampScrollOffsets(document_->documentElement(), &reclamped)) {
                    for (auto* elem : reclamped) dispatchScrollEvent(elem);
                    markAppBaseDirty();
                }
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

            (void)webAnimationManager_.takeFinishedEvents();

            syncAllIframeBoxes();
            deliverMediaQueryChangesAllRealms();

            bool promotedSetChanged = (promotedElements_ != basePromotedSet_);
            if (baseWasDirty || promotedSetChanged || !baseValid_) {
                uiDirty_ = true;
                appBaseDirty_ = true;
            }
        }
    }

    if (framePresenter_->isRasterIdle()) {
        bool uiThrottled = (now - lastUIRenderMs_ < uiFrameIntervalMs_);
        bool promotedActive = layoutPipeline_->promotedActive();
        if ((uiDirty_ || !hasRenderedOnce_ || promotedActive) && !uiThrottled) {
            stageSystemPanelCanvases();
            updateSelectionSnapshot();

            auto& backBuf = framePresenter_->backBuffer();
            auto rsnap = buildRasterSnapshot();

            const std::unordered_set<dom::Element*>* pset =
                promotedElements_.empty() ? nullptr : &promotedElements_;

            bool scrollOrInsetChanged =
                rsnap.scrollY != baseScrollY_ ||
                rsnap.insetTop != baseInsetTop_ ||
                rsnap.insetRight != baseInsetRight_ ||
                rsnap.insetBottom != baseInsetBottom_;
            bool baseNeedsRecord = appBaseDirty_ || scrollOrInsetChanged ||
                                   !baseValid_ || !hasRenderedOnce_;

            if (baseNeedsRecord) {
                recordAppLayers(baseCommands_,
                                rsnap.vpWidth, rsnap.vpHeight,
                                rsnap.insetTop, rsnap.insetRight, rsnap.insetBottom,
                                rsnap.scrollY, pset, /*promotedOnly=*/false);
                basePromotedSet_ = promotedElements_;
                baseScrollY_ = rsnap.scrollY;
                baseInsetTop_ = rsnap.insetTop;
                baseInsetRight_ = rsnap.insetRight;
                baseInsetBottom_ = rsnap.insetBottom;
                baseValid_ = true;
                appBaseDirty_ = false;
            }

            if (pset) {
                recordAppLayers(backBuf.promotedCommands,
                                rsnap.vpWidth, rsnap.vpHeight,
                                rsnap.insetTop, rsnap.insetRight, rsnap.insetBottom,
                                rsnap.scrollY, pset, /*promotedOnly=*/true);
            } else {
                backBuf.promotedCommands.clear();
            }

            backBuf.appInsetTop = rsnap.insetTop;
            backBuf.appContentW = rsnap.vpWidth - rsnap.insetRight;
            backBuf.appContentH = rsnap.vpHeight - rsnap.insetTop
                                  - rsnap.insetBottom;
            recordSystemPanelLayers(backBuf.systemCommands,
                                    rsnap.vpWidth, rsnap.vpHeight);

            processPendingIframeReloads();
            processPendingWindowHosts();
#if BRO_WITH_BRONZE
            bro::bronze_host::drainHostWindowMessages();
#endif

            if (iframeSyncNeeded_) {
                syncIframes();
                iframeSyncNeeded_ = false;
            }
            recordIframeLayers();
            recordWindowHostLayers();

            framePresenter_->signalRender(rsnap);
            uiDirty_ = false;
            hasRenderedOnce_ = true;
            lastUIRenderMs_ = now;
        }
    }

    auto layers = framePresenter_->currentLayers();

    for (auto& layer : layers.appLayers) {
        if (layer.type != UILayer::Canvas) continue;
        if (auto* cs = canvasSceneById(layer.canvasSceneId))
            cs->prepareAndSignal();
    }

    for (auto& layer : layers.appLayers) {
        if (layer.type != UILayer::Canvas) continue;
        if (auto* cs = canvasSceneById(layer.canvasSceneId))
            cs->consumeFence();
    }

    accumRasterMs_ += (util::currentTimeMs() - tRaster) - layoutWaitMs;
    accumLayoutMs_ += layoutWaitMs;

    double tGpu = util::currentTimeMs();

    for (auto& cs : canvasScenes_) {
        cs->setViewportScroll(scrollY_);
        cs->checkDetached();
    }
    for (auto& cs : canvasScenes_) {
        if (!cs->isDetached()) continue;
        if (auto* el = static_cast<dom::Element*>(cs->backingElement()))
            el->setCanvasScene(nullptr);
    }
    for (auto it = canvasScenes_.begin(); it != canvasScenes_.end(); ) {
        if ((*it)->isDetached()) {
            canvasSceneRegistry_.erase((*it)->sceneId());
            canvasScenesDetached_.push_back(std::move(*it));
            it = canvasScenes_.erase(it);
        } else {
            ++it;
        }
    }

    glViewport(0, 0, viewportWidth_, viewportHeight_);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    compositeLayers(layers.appLayers, 0,
                    layers.appInsetTop, layers.appContentW,
                    layers.appContentH);

    compositeLayers(layers.systemLayers);

    compositeWindowHosts();

    webgl::WebGL2RenderingContext::invalidateCurrent();

    accumGpuMs_ += util::currentTimeMs() - tGpu;

    if (window_) window_->swapWindow();

    {
        double capMs = frameCapIntervalMs_;
        if (!windowFocused_ && !anyWindowHostFocused())
            capMs = std::max(capMs, 1000.0 / kUnfocusedFps);
        if (capMs > 0.0) {
            double elapsed = util::currentTimeMs() - frameStart;
            double sleepMs = capMs - elapsed;
            if (sleepMs > 0.5) {
                std::this_thread::sleep_for(std::chrono::microseconds(
                    static_cast<int64_t>(sleepMs * 1000.0)));
            }
        }
    }

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
        phaseJsMs_      = accumJsMs_      / n;
        phaseLayoutMs_  = accumLayoutMs_  / n;
        phaseRasterMs_  = accumRasterMs_  / n;
        phaseGpuMs_     = accumGpuMs_     / n;
        phaseGlStateMs_ = accumGlStateMs_ / n;
        phaseDrawMs_    = accumDrawMs_    / n;
        phaseUploadMs_  = accumUploadMs_  / n;
        accumJsMs_ = accumLayoutMs_ = accumRasterMs_ = accumGpuMs_ = accumGlStateMs_ = 0.0;
        accumDrawMs_ = accumUploadMs_ = 0.0;
        statsAccumMs_ = 0.0;
        statsFrameCount_ = 0;
        statsMinFrameMs_ = 999.0;
        statsMaxFrameMs_ = 0.0;
        uiDirty_ = true;

        updateSystemPerf(statsFps_, statsFrameTimeMs_,
                         phaseJsMs_, phaseLayoutMs_,
                         phaseRasterMs_, phaseGpuMs_,
                         phaseDrawMs_,
                         viewportWidth_, viewportHeight_);
    }
}

} // namespace bro::engine
