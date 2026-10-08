#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/overflow.h"
#include "engine/terminal_layers.h"
#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "webgl/webgl2_context.h"
#include "platform/sdl_window.h"
#include "util/time.h"
#include "bronze_host/host_window_open.h"
#include "engine/engine_drm.h"
#include "render/software_cursor.h"
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
#endif

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
    s.scale       = deviceScale_.render;
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
            updateDocumentHeight();

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
                ++frameStats_.baseRecords;
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
            bro::bronze_host::drainHostWindowMessages();

            if (iframeSyncNeeded_) {
                syncIframes();
                iframeSyncNeeded_ = false;
            }
            recordIframeLayers();
            recordWindowHostLayers();
            // Each <terminal> whose paint changed, into its own layer.
            if (terminalLayers_) terminalLayers_->record(rsnap.scale);

            framePresenter_->signalRender(rsnap);
            uiDirty_ = false;
            hasRenderedOnce_ = true;
            lastUIRenderMs_ = now;
        }
    }

    auto layers = framePresenter_->currentLayers();

    frameStats_.accumRasterMs += (util::currentTimeMs() - tRaster) - layoutWaitMs;
    frameStats_.accumLayoutMs += layoutWaitMs;

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

    // Bring each composited canvas up to date: replay what its script drew
    // since the last frame (on the GPU, leaving its image ready to sample).
    for (const auto& layer : layers.appLayers) {
        if (const auto* canvas = std::get_if<render::CanvasLayerSource>(&layer.content))
            if (auto* cs = canvasSceneById(canvas->sceneId)) cs->rasterize();
    }

    beginFrameComposite();
    if (drmCtx_) drmCtx_->clientLayersComposited = false;
    compositeLayers(layers.appLayers, layers.appInsetTop);
    compositeRemainingClientWindows();

    compositeLayers(layers.systemLayers);

    compositeWindowHosts();

    if (displayMode_ == DisplayMode::Drm && cursorVisible_ && !lockedElement_.get()) {
        std::string shape = resolvedCursor_;
#if BRO_WITH_COMPOSITOR
        // The client's cursor while the pointer is on a client (or a window
        // is being dragged); over the shell, the shell's.
        if (drmCtx_ && drmCtx_->compositor &&
            (drmCtx_->pointerOnClient || drmCtx_->compositor->isDragActive() || !isShellApp())) {
            auto c = drmCtx_->compositor->cursor();
            if (c.hidden) shape = "none";
            else if (!c.shape.empty()) shape = c.shape;
        }
#endif
        if (shape != "none") {
            if (SkCanvas* canvas = frameSegmentCanvas()) {
                float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
                float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
                render::drawSoftwareCursor(canvas, lastMouseX_ * sx, lastMouseY_ * sy, shape, deviceScale_.render);
            }
        }
    }

    frameStats_.accumGpuMs += util::currentTimeMs() - tGpu;

    presentCurrentFrame();

    releaseClientWindowFrames();

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

    if (frameStats_.addFrame(util::currentTimeMs() - frameStart)) {
        if (systemPerfVisible_) uiDirty_ = true;
    }
}

} // namespace bro::engine
