#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/layout_pipeline.h"

#include "dom/document.h"
#include "dom/element.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "layout/font_manager.h"
#include "platform/sdl_window.h"
#include "render/raster_renderer.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"
#include "util/log.h"
#include "util/time.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <include/gpu/ganesh/GrDirectContext.h>

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

    layout::FontManager layoutFontManager;
    layout::SkiaTextMetrics layoutTextMetrics(layoutRenderer.get(), &layoutFontManager);

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
            // resolveStyles() must run BEFORE tick(): animations and transitions
            // are *registered* inside the cascade (onStyleChange), so a tick
            // that runs first wouldn't see the entry an event-driven frame just
            // created — animActive would publish as false, the main loop would
            // skip signalling layout the following frame, and the animation
            // would freeze on its first applied value (e.g. 2048's tile-pop-in
            // stuck at scale(0), tiles invisible). Re-trigger after completion
            // is prevented by the previousName memo in AnimationManager.
            document_->resolveStyles();
            bool animActive = transitionManager_.tick(now) | animationManager_.tick(now);
            layoutPipeline_->setAnimationsActive(animActive);
            // performLayout() rebuilds the persistent layout tree when
            // structureDirty_ is set and clears the flag itself.
            document_->performLayout(
                static_cast<float>(snap.vpWidth - snap.insetRight),
                static_cast<float>(snap.vpHeight - snap.insetTop - snap.insetBottom),
                layoutTextMetrics);
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
        auto& backBuf = framePresenter_->backBuffer();
        backBuf.appLayers.clear();
        backBuf.systemLayers.clear();

        rasterRenderer->grContext()->resetContext();
        rasterRenderer->beginFrame(snap.vpWidth, snap.vpHeight);

        replayAppLayers(rasterRenderer.get(), backBuf.appCommands,
                        htmlSurfacePool_, htmlSurfacePoolW_, htmlSurfacePoolH_,
                        snap.vpWidth, snap.vpHeight,
                        backBuf.appLayers);

        replaySystemPanelLayers(rasterRenderer.get(), backBuf.systemCommands,
                                systemSurfacePool_, systemSurfacePoolW_,
                                systemSurfacePoolH_,
                                snap.vpWidth, snap.vpHeight,
                                backBuf.systemLayers);

        rasterRenderer->endFrame();

        // GL fence — guarantees all GPU commands are visible before the main
        // thread samples textures for compositing.
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        glFlush();  // ensure fence is submitted to GPU command stream

        framePresenter_->publishResult(fence);
    }

    // Cleanup
    for (auto& ps : htmlSurfacePool_)   rasterRenderer->destroyGPUSurface(ps);
    htmlSurfacePool_.clear();
    for (auto& ps : systemSurfacePool_) rasterRenderer->destroyGPUSurface(ps);
    systemSurfacePool_.clear();
    rasterRenderer.reset();
    SDL_GL_MakeCurrent(window_->getSDLWindow(), nullptr);
    LOG_INFO("Raster thread stopped");
}

} // namespace bro::engine
