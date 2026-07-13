#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"

#include "dom/document.h"
#include "dom/element.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "platform/sdl_window.h"
#include "render/raster_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "util/log.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <algorithm>
#include <bit>

namespace bro::engine {

// ---------------------------------------------------------------------------
// Layout thread — owns style resolution + layout computation. Reads DOM tree
// (read-only after JS phase), writes computedStyle_ + layoutBox_ on each
// element. No GL needed — uses CPU-only RasterRenderer for text measurement.
// ---------------------------------------------------------------------------

void Engine::layoutThreadFunc() {
    auto layoutRenderer = std::make_unique<render::RasterRenderer>();

    // Register custom fonts on the layout renderer so text measurement uses them
    for (auto& font : loadedFonts_) {
        layoutRenderer->registerCustomFont(font.family, font.data.data(),
                                           font.data.size(), font.weight, font.italic);
    }

    layout::SkiaTextMetrics layoutTextMetrics(layoutRenderer.get());

    LOG_INFO("Layout thread started");

    while (layoutPipeline_->waitForRequest()) {
        layoutPipeline_->markBusy();
        auto snap = layoutPipeline_->loadSnapshot();

        if (document_) {
            layout::ElementRefAdapter::setHoveredElement(snap.hoveredElement);
            double now = util::currentTimeMs();
            document_->setTransitionManager(&transitionManager_, now);
            auto& kfs = document_->cascade().keyframes();
            animationManager_.setKeyframes(&kfs);
            document_->setAnimationManager(&animationManager_);
            int contentW = snap.vpWidth - snap.insetRight;
            int contentH = snap.vpHeight - snap.insetTop - snap.insetBottom;

            // resolveStyles() must run BEFORE tick(): animations and transitions
            // are *registered* inside the cascade (onStyleChange), so a tick
            // that runs first wouldn't see the entry an event-driven frame just
            // created — animActive would publish as false, the main loop would
            // skip signalling layout the following frame, and the animation
            // would freeze on its first applied value (e.g. 2048's tile-pop-in
            // stuck at scale(0), tiles invisible). Re-trigger after completion
            // is prevented by the previousName memo in AnimationManager.
            document_->resolveStyles();

            // Decided AFTER resolveStyles so it sees layoutDirty_ promotions:
            // a paint-only (hover) frame whose re-resolve turned up a real
            // geometry change is promoted inside resolveStyles and lands here as
            // isLayoutDirty(). A frame whose only change is a promoted
            // (transform/opacity) animation, or a hover that touched only
            // paint properties, leaves layoutDirty_ false and skips the full
            // 4k-element layoutTree() pass that otherwise caps fps. A
            // viewport/inset resize is picked up by the content-dimension compare.
            bool layoutAffecting = document_->isLayoutDirty() ||
                                   document_->isStructureDirty() ||
                                   !document_->layoutRoot() ||
                                   contentW != lastLayoutContentW_ ||
                                   contentH != lastLayoutContentH_;

            bool animActive = transitionManager_.tick(now) | animationManager_.tick(now);

            // Route this tick's active animations. An element whose active
            // animations/transitions are confined to transform/opacity becomes
            // a compositor layer: it does NOT mark the document dirty (so the
            // expensive base record is skipped), only re-composited with its
            // fresh transform. Anything else marks the element dirty so the
            // base re-records exactly as before. The ticks deferred markDirty
            // to here so this decision — which needs both managers — is made in
            // one place, after both have advanced.
            promotedElements_.clear();
            auto routePromotion = [&](dom::Element* e) {
                if (isTransformOpacityOnly(e, animationManager_, transitionManager_))
                    promotedElements_.insert(e);
                else {
                    // A non-promoted animation (width/left/color/…) can change
                    // layout, so it dirties the element and forces the pass.
                    e->markDirty();
                    layoutAffecting = true;
                }
            };
            for (auto* e : transitionManager_.activeThisTick()) routePromotion(e);
            for (auto* e : animationManager_.activeThisTick())  routePromotion(e);

            layoutPipeline_->setAnimationsActive(animActive);
            layoutPipeline_->setPromotedActive(!promotedElements_.empty());

            // Skip the full layoutTree() pass on a promoted-only frame — the
            // layout is identical to last frame, only paint-time transforms
            // changed. performLayout() rebuilds the persistent layout tree when
            // structureDirty_ is set and clears the flag itself.
            if (layoutAffecting) {
                document_->performLayout(static_cast<float>(contentW),
                                         static_cast<float>(contentH),
                                         layoutTextMetrics);
                lastLayoutContentW_ = contentW;
                lastLayoutContentH_ = contentH;
            }
            document_->clearDirty();
        }

        layoutPipeline_->publishDone();
    }

    layoutRenderer.reset();
    LOG_INFO("Layout thread stopped");
}

// ---------------------------------------------------------------------------
// Raster thread — owns the GPU surface pool + Skia/Ganesh context. Replays
// command buffers recorded by the main thread; never reads the DOM. Produces
// GPU textures and signals the main thread via FramePresenter.
// ---------------------------------------------------------------------------

void Engine::rasterThreadFunc() {
    // Main thread already created rasterGLContext_ (macOS/AppKit requirement);
    // we MakeCurrent it here, which is a thread-local GL operation. Main is
    // parked in run() on rasterReady_ so no other wgl*Context call can overlap
    // with this one (Windows/NVIDIA requirement).
    SDL_GL_MakeCurrent(window_->getSDLWindow(), rasterGLContext_);
    rasterReady_.store(true, std::memory_order_release);
    rasterReady_.notify_one();

    // Per-thread Skia + Ganesh context. Skia GPU contexts aren't thread-safe.
    auto rasterRenderer = std::make_unique<render::SkiaRenderer>(*gl_);
    if (!rasterRenderer->grContext()) {
        LOG_ERROR("Raster thread: SkiaRenderer failed to create GrDirectContext");
        return;
    }
    for (auto& font : loadedFonts_) {
        rasterRenderer->registerCustomFont(font.family, font.data.data(),
                                           font.data.size(), font.weight, font.italic);
    }

    LOG_INFO("Raster thread started");

    while (framePresenter_->waitForRequest()) {
        framePresenter_->markBusy();
        auto snap = framePresenter_->loadSnapshot();

        // Same slot the main thread wrote command buffers into (1 - front_).
        // The state machine guarantees main's writes happened-before this read.
        // The back index also selects which surface-pool copy we draw into, so
        // the compositor (reading the front pool) never samples surfaces we are
        // clearing+redrawing here. front_ is stable for the whole request — main
        // only flips it in consumeIfReady, which runs after we publish — so the
        // two 1-front_ reads (here and in backBuffer()) always agree.
        int back = framePresenter_->backIndex();
        auto& backBuf = framePresenter_->backBuffer();
        backBuf.appLayers.clear();
        backBuf.systemLayers.clear();

        // Destroy any iframe surface the main thread orphaned (a reload whose
        // rebuild failed) — it belongs to THIS context. Before resetContext(),
        // so the raw glDelete* below don't leave Ganesh's cached GL state stale.
        drainIframeSurfaceFrees(rasterRenderer.get());

        rasterRenderer->grContext()->resetContext();
        rasterRenderer->beginFrame(snap.vpWidth, snap.vpHeight);

        // App layer surfaces are content-sized (viewport minus engine
        // insets); the main thread's compositor places them at (0, insetTop).
        int contentW = std::max(1, snap.vpWidth - snap.insetRight);
        int contentH = std::max(1, snap.vpHeight - snap.insetTop - snap.insetBottom);
        // Base commands come from the engine's single cross-frame cache
        // (rebuilt by main only on a real base change, always while we're idle);
        // the promoted subtree commands are this slot's fresh per-frame buffer,
        // replayed on top. Both reads are ordered after main's writes by the
        // FrameWorker request handshake.
        replayAppLayers(rasterRenderer.get(), baseCommands_,
                        htmlSurfacePool_[back], htmlSurfacePoolW_[back],
                        htmlSurfacePoolH_[back],
                        contentW, contentH,
                        backBuf.appLayers,
                        &backBuf.promotedCommands);

        replaySystemPanelLayers(rasterRenderer.get(), backBuf.systemCommands,
                                systemSurfacePool_[back], systemSurfacePoolW_[back],
                                systemSurfacePoolH_[back],
                                snap.vpWidth, snap.vpHeight,
                                backBuf.systemLayers);

        // Replay each iframe sub-document into its box-sized surface (raster
        // thread) → IframeDoc::fboTexture, which the compositor samples for the
        // UILayer::Iframe quads recorded during the app pass.
        replayIframeLayers(rasterRenderer.get());

        rasterRenderer->endFrame();

        // GL fence — guarantees all GPU commands are visible before the main
        // thread samples textures for compositing.
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();  // ensure fence is submitted to GPU command stream

        framePresenter_->publishResult(fence);
    }

    // Cleanup — both double-buffered pool copies.
    for (int i = 0; i < 2; ++i) {
        for (auto& ps : htmlSurfacePool_[i])   rasterRenderer->destroyGPUSurface(ps);
        htmlSurfacePool_[i].clear();
        for (auto& ps : systemSurfacePool_[i]) rasterRenderer->destroyGPUSurface(ps);
        systemSurfacePool_[i].clear();
    }
    // Iframe sub-document surfaces live on this context too (replayIframeLayers
    // created them), so this is the LAST point they can be released: ~Engine()
    // destroys the IframeDocs on the main thread, after this thread has joined
    // and rasterGLContext_ is gone — dropping the sk_sp and the FBO with no
    // context to free them against. Reading iframeDocs_ here is race-free: the
    // main thread is blocked in rasterThread_.join() (the same reason the pools
    // above are safe to touch).
    for (auto& d : iframeDocs_) {
        if (!d) continue;
        rasterRenderer->destroyGPUSurface(d->surface);
        d->surfW = d->surfH = 0;
        d->fboTexture = 0;
    }
    drainIframeSurfaceFrees(rasterRenderer.get());

    rasterRenderer.reset();
    SDL_GL_MakeCurrent(window_->getSDLWindow(), nullptr);
    LOG_INFO("Raster thread stopped");
}

} // namespace bro::engine
