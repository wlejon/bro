#include "engine/engine.h"
#include "engine/frame_trace.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"
#include "engine/overflow.h"
#include "engine/terminal_layers.h"
#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "webgl/webgl2_context.h"
#include "platform/window.h"
#include "util/time.h"
#include "bronze_host/host_window_open.h"
#include "engine/engine_drm.h"
#include "render/software_cursor.h"
#include "render/vulkan_presenter.h"
#if BRO_WITH_DMABUF
#include "render/kms_direct_presenter.h"
#endif
#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
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
        frameTrace_->current().layoutWaitMs = layoutWaitMs;
        if (layoutClaimed) {
            traceLayoutClaimed();
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
            // An animation the compositor cannot carry changed the base this
            // pass; the pass cleared the document's dirty flag behind it.
            if (baseWasDirty || promotedSetChanged || !baseValid_ || layoutPipeline_->baseAnimated()) {
                uiDirty_ = true;
                appBaseDirty_ = true;
            }
        }
    }

    const double tRecord = util::currentTimeMs();
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
                frameTrace_->current().recorded = true;
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
            backBuf.vpWidth = rsnap.vpWidth;
            backBuf.vpHeight = rsnap.vpHeight;
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
            traceRasterSignalled();
            uiDirty_ = false;
            hasRenderedOnce_ = true;
            lastUIRenderMs_ = now;

            // Windowed, the raster just asked for is shown in this frame when
            // it is done in the first half of the refresh: the present is
            // paced by the window system anyway, so waiting costs nothing,
            // and otherwise what was recorded now (a key's echo) shows a
            // refresh later. A raster that takes longer is shown next frame,
            // as before.
            if (displayMode_ == DisplayMode::Windowed) {
                const double period = frameTrace_->refreshPeriodMs() > 0.0 ? frameTrace_->refreshPeriodMs() : 1000.0 / 60.0;
                const double waitMs = frameStart + period * 0.5 - util::currentTimeMs();
                if (waitMs > 0.0 && framePresenter_->waitForRaster(waitMs) && framePresenter_->consumeIfReady())
                    traceRasterConsumed();
            }
        }
    }

    auto layers = framePresenter_->currentLayers();

    frameTrace_->current().recordMs = util::currentTimeMs() - tRecord;
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

    // A window is shown at a new size only once a frame drawn at that size is
    // ready. The raster runs behind the main thread, so for the first frames
    // after a resize (a maximize, a restore) the newest layer set is still
    // the one recorded at the old viewport, while the swapchain follows the
    // window's new size: presented, that would put the old frame in a corner
    // of a black one. Until the set catches up nothing is presented, so the
    // window keeps its last buffer and its compositor keeps showing it at the
    // old size (and scales it, if it animates the change). A raster that
    // never catches up (the size changing every frame) is presented anyway
    // after kResizeHoldMaxMs.
    bool holdForResize = false;
    if (displayMode_ == DisplayMode::Windowed && layers.vpWidth > 0 &&
        (layers.vpWidth != viewportWidth_ || layers.vpHeight != viewportHeight_)) {
        if (resizeHoldSinceMs_ <= 0.0) resizeHoldSinceMs_ = now;
        holdForResize = now - resizeHoldSinceMs_ < kResizeHoldMaxMs;
    } else {
        resizeHoldSinceMs_ = 0.0;
    }

    FrameRecord& rec = frameTrace_->current();
    if (!holdForResize) {
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
        compositeDragIcon();

        // Under DRM the pointer goes on the cursor plane where it can, else
        // into the frame (and into its key: a moved cursor is a new frame).
        if (displayMode_ == DisplayMode::Drm && !drmPlaceHardwareCursor() && cursorVisible_ && !lockedElement_.get()) {
            const std::string shape = screenCursorShape();
            if (shape != "none") {
                if (SkCanvas* canvas = frameSegmentCanvas()) {
                    float sx = static_cast<float>(frameCompositeW_) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
                    float sy = static_cast<float>(frameCompositeH_) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
                    render::drawSoftwareCursor(canvas, lastMouseX_ * sx, lastMouseY_ * sy, shape, deviceScale_.render);
                    frameKeyAdd(std::hash<std::string>{}(shape));
                    frameKeyAdd(static_cast<uint64_t>(std::lround(lastMouseX_ * sx)) << 32 ^
                                static_cast<uint32_t>(std::lround(lastMouseY_ * sy)));
                }
            }
        }

        frameStats_.accumGpuMs += util::currentTimeMs() - tGpu;

        const double tPresent = util::currentTimeMs();
        rec.compositeMs = tPresent - tGpu;
        rec.presentAtMs = tPresent;
        // An unchanged frame is held: nothing presented, no GPU work.
        const bool presented = presentCurrentFrame(/*mayHold=*/true);
        rec.presentMs = presented ? util::currentTimeMs() - tPresent : 0.0;
        if (!presented) rec.presentAtMs = 0.0;
#if BRO_WITH_DMABUF
        if (auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr) {
            rec.gpuWaitMs = presented ? kms->lastPresentTiming().gpuWaitMs : 0.0;
            rec.flipWaitMs = presented ? kms->lastPresentTiming().flipWaitMs : 0.0;
        }
#endif

        releaseClientWindowFrames();
    } else {
        // No present to pace the loop on: wait a little for the raster.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

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
                rec.pacingWaitMs += sleepMs;
            }
        }
    }

    if (frameStats_.addFrame(util::currentTimeMs() - frameStart)) {
        if (systemPerfVisible_) uiDirty_ = true;
    }
}

} // namespace bro::engine
