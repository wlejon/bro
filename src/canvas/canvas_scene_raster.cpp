// CanvasScene's raster side: the shared canvas worker (CanvasRasterThread),
// the per-scene work it runs, command replay onto the SkSurface, and the
// compositing upload. The recording API that fills the command buffer lives in
// canvas_scene.cpp and canvas_scene_state.cpp.

#include "canvas/canvas_scene.h"
#include "render/gl_context.h"
#include "render/skia_backend.h"
#include "util/log.h"

#include <SDL3/SDL.h>

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkM44.h>
#include <include/core/SkPixmap.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <iterator>

namespace bro::canvas {

// ---------------------------------------------------------------------------
// Threading
// ---------------------------------------------------------------------------

// Worker-thread render body: ensure the surface, replay staged commands, and
// refresh the snapshot if one was requested. Runs ON the shared worker thread
// (CanvasRasterThread::threadFunc), where this scene's GrContext is current.
void CanvasScene::renderOnWorker(GrDirectContext* grctx, int w, int h) {
    grContext_ = grctx;          // pin the surface to the worker's context
    ensureSurface(w, h);

    if (grContext_) grContext_->resetContext();

    flushStagedCommands();

    if (dirty_) {
        dirty_ = false;
        if (grContext_) {
            grContext_->flushAndSubmit();
        }
    }

    // Snapshot readback (canvas-as-source for drawImage / getImageData). Done
    // here because the surface is Ganesh-backed against this worker's GrContext
    // — readPixels from the main thread would cross GL contexts. The bytes copy
    // into a portable raster SkImage so destination scenes can blit it.
    if (snapshotRequested_.load(std::memory_order_acquire)) {
        int sw = surfWidth_;
        int sh = surfHeight_;
        if (surface_ && sw > 0 && sh > 0) {
            if (grContext_) grContext_->resetContext();
            snapshot_.assign(static_cast<size_t>(sw) * sh * 4, 0);
            auto info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
            bool ok = surface_->readPixels(info, snapshot_.data(), sw * 4, 0, 0);
            if (ok) {
                auto data = SkData::MakeWithCopy(snapshot_.data(), snapshot_.size());
                snapshotImage_ = SkImages::RasterFromData(info, data, sw * 4);
                snapshotW_ = sw;
                snapshotH_ = sh;
                snapshotValid_ = true;
                snapshotImageValid_ = static_cast<bool>(snapshotImage_);
            } else {
                snapshotImage_.reset();
                snapshotValid_ = false;
                snapshotImageValid_ = false;
            }
        } else {
            snapshot_.clear();
            snapshotImage_.reset();
            snapshotValid_ = false;
            snapshotImageValid_ = false;
        }
        snapshotRequested_.store(false, std::memory_order_release);
    }
}

// Worker-thread teardown: free GPU resources on the context that created them.
void CanvasScene::releaseGpuResources() {
    surface_.reset();
    snapshotImage_.reset();
    snapshotImageValid_ = false;
    snapshotValid_ = false;
    if (grContext_) grContext_->flushAndSubmit();
    gpuFBO_ = 0;
    glTexture_ = 0;
    surfWidth_ = surfHeight_ = 0;
    texWidth_ = texHeight_ = 0;
    grContext_ = nullptr;
}

void CanvasScene::prepareAndSignal() {
    if (!threaded_ || !rasterThread_) return;

    // Check if element was removed from the DOM. Offscreen canvases (created
    // via document.createElement and never appended) read as orphaned from
    // frame one even though they're being used as sprite atlases — defer until
    // the canvas has been seen attached at least once.
    if (detachedCb_) {
        // The backing Element may have been freed (deferred-free / pointer
        // reuse) since this scene was last touched. backingElementAlive() is a
        // pointer-value liveness check that never dereferences it, so it is
        // safe even on a dangling pointer — this is the guard for the
        // use-after-free that crashed here on rapid canvas churn.
        if (!backingElementAlive()) { onElementFinalized(); return; }
        bool orphaned = detachedCb_(detachedUd_);
        if (!orphaned) everAttached_ = true;
        if (orphaned && everAttached_) {
            detached_ = true;
            return;
        }
    }

    // Query element layout position (main thread, DOM access). The displayed
    // size still comes from layout, but the bitmap (surface) size uses
    // queryLayoutWidth/Height so an intrinsic canvas.width/height attribute
    // beats the layout box.
    float layoutX = 0, layoutY = 0, layoutW = 0, layoutH = 0;
    if (layoutCb_) {
        layoutCb_(layoutUd_, layoutX, layoutY, layoutW, layoutH);
    }

    screenX_ = layoutX;
    screenY_ = layoutY - viewportScrollY_;

    int canvasW = queryLayoutWidth();
    int canvasH = queryLayoutHeight();
    if (canvasW <= 0 || canvasH <= 0) return;

    // Only rasterize if there's work to do (commands or resize).
    bool needsResize = (canvasW != surfWidth_ || canvasH != surfHeight_);
    if (commands_.empty() && !needsResize) return;

    // Swap commands to the staged buffer the worker replays, then rasterize
    // synchronously on the shared worker (blocks until the fence is consumed).
    if (!commands_.empty()) std::swap(commands_, stagedCommands_);
    rasterThread_->render(this, canvasW, canvasH);
}

void CanvasScene::flushSync() {
    if (!threaded_ || !rasterThread_) {
        flushCommands();
        return;
    }
    int cw = queryLayoutWidth();
    int ch = queryLayoutHeight();
    if (cw <= 0 || ch <= 0) return;
    // Swap any pending commands; render() also services a pending snapshot.
    if (!commands_.empty()) std::swap(commands_, stagedCommands_);
    rasterThread_->render(this, cw, ch);
}

// ---------------------------------------------------------------------------
// CanvasRasterThread — one persistent canvas-raster worker (shared by scenes)
// ---------------------------------------------------------------------------

void CanvasRasterThread::start(SDL_GLContext glCtx, SDL_Window* win) {
    if (started_ || !glCtx || !win) return;
    glCtx_ = glCtx;
    started_ = true;
    ready_ = false;
    shutdown_ = false;
    hasJob_ = false;
    thread_ = std::thread(&CanvasRasterThread::threadFunc, this, win);
    // Block until the worker has MakeCurrent'd its context — the Windows/NVIDIA
    // "no concurrent wgl*Context against the same HDC" serialization. Created
    // once here, while quiescent, so it never overlaps the raster thread.
    std::unique_lock<std::mutex> lk(m_);
    cv_.wait(lk, [this] { return ready_; });
}

void CanvasRasterThread::stop() {
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(m_);
        shutdown_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (glCtx_) {
        SDL_GL_DestroyContext(glCtx_);
        glCtx_ = nullptr;
    }
    started_ = false;
}

void CanvasRasterThread::render(CanvasScene* scene, int w, int h) {
    if (!started_ || !scene) return;
    submitJob(scene, w, h, JobKind::Render);
}

void CanvasRasterThread::releaseScene(CanvasScene* scene) {
    if (!started_ || !scene) return;
    submitJob(scene, 0, 0, JobKind::Release);
}

void CanvasRasterThread::submitJob(CanvasScene* scene, int w, int h, JobKind kind) {
    {
        std::unique_lock<std::mutex> lk(m_);
        if (shutdown_) return;
        job_ = scene; jobW_ = w; jobH_ = h; jobKind_ = kind;
        hasJob_ = true;
        cv_.notify_all();
        cv_.wait(lk, [this] { return !hasJob_ || shutdown_; });
        doneFence_ = nullptr;
    }
}

void CanvasRasterThread::threadFunc(SDL_Window* win) {
    (void)win;
    // Signal the main thread that MakeCurrent is done before any further work.
    {
        std::lock_guard<std::mutex> lk(m_);
        ready_ = true;
    }
    cv_.notify_all();

    grContext_ = render::SkiaRenderer::createGrContext();
    LOG_INFO("Canvas raster thread started");

    std::unique_lock<std::mutex> lk(m_);
    while (true) {
        cv_.wait(lk, [this] { return hasJob_ || shutdown_; });
        if (shutdown_) break;
        CanvasScene* s = job_;
        const int w = jobW_, h = jobH_;
        const JobKind kind = jobKind_;
        lk.unlock();

        if (s) {
            if (kind == JobKind::Render)
                s->renderOnWorker(grContext_.get(), w, h);
            else
                s->releaseGpuResources();
        }

        lk.lock();
        doneFence_ = nullptr;
        hasJob_ = false;
        cv_.notify_all();
    }
    doneFence_ = nullptr;
    lk.unlock();

    if (grContext_) {
        grContext_->flushAndSubmit();
        grContext_.reset();
    }
    LOG_INFO("Canvas raster thread stopped");
}

void CanvasScene::stageCommandsForRaster() {
    if (commands_.empty()) return;
    if (stagedCommands_.empty()) {
        std::swap(commands_, stagedCommands_);
    } else {
        stagedCommands_.insert(stagedCommands_.end(),
            std::make_move_iterator(commands_.begin()),
            std::make_move_iterator(commands_.end()));
        commands_.clear();
    }
}

// Streaming-buffer canvas fast path. When a frame's command buffer consists
// only of putImageData calls, the SkCanvas replay (paint setup, transform
// save/restore, draw-image submission) is pure overhead — the final state of
// the surface is just the bytes from the *last* putImageData. We can skip
// directly to a bulk pixel upload via SkSurface::writePixels, which on a
// Ganesh GPU surface hits a glTexSubImage2D-style path with no draw pipeline
// activity. This is the dominant pattern for streaming visualizations
// (noise fields, audio waveforms, spectrograms, voxel mini-maps).
//
// Conservative check: every command must be kPutImageData. If any other op
// is present (paths, text, transforms, clears, etc.) we fall back to the
// regular replay. The last putImageData must also fit within the surface;
// partial-region writes are handled correctly by writePixels' offset args.
//
// Returns true if the fast path consumed the commands; the caller should
// then clear the buffer and return without running the normal replay.
static bool tryStreamingPutImageDataFastPath(SkSurface* surface,
                                             std::vector<CanvasCmd>& cmds)
{
    if (!surface || cmds.empty()) return false;
    int lastPut = -1;
    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].type != CanvasCmd::kPutImageData) return false;
        if (!cmds[i].src.isEmpty()) return false;
        lastPut = static_cast<int>(i);
    }
    if (lastPut < 0) return false;

    const CanvasCmd& cmd = cmds[lastPut];
    if (!cmd.img) return false;
    SkPixmap pm;
    if (!cmd.img->peekPixels(&pm)) return false;

    const int dx = static_cast<int>(cmd.p[0]);
    const int dy = static_cast<int>(cmd.p[1]);
    const int sw = surface->width();
    const int sh = surface->height();
    // writePixels will clip silently to the surface, but reject obviously bad
    // offsets so we do not mask an engine bug with an empty write.
    if (dx + pm.width() <= 0 || dy + pm.height() <= 0 || dx >= sw || dy >= sh) {
        return false;
    }
    surface->writePixels(pm, dx, dy);
    return true;
}

void CanvasScene::replayOne(SkCanvas* c, CanvasCmd& cmd) {
    switch (cmd.type) {
    case CanvasCmd::kFillRect:
    case CanvasCmd::kStrokeRect:
        c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), cmd.paint);
        break;
    case CanvasCmd::kClearRect: {
        SkPaint clr;
        clr.setBlendMode(SkBlendMode::kClear);
        c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), clr);
        break;
    }
    case CanvasCmd::kStrokePath:
    case CanvasCmd::kFillPath:
        c->drawPath(cmd.path, cmd.paint);
        break;
    case CanvasCmd::kClipPath:
        c->clipPath(cmd.path, true);
        break;
    case CanvasCmd::kFillText:
    case CanvasCmd::kStrokeText: {
        bool scaled = cmd.p[2] > 0.0f && cmd.p[2] < 1.0f;
        if (scaled) {
            c->save();
            c->translate(cmd.p[3], 0.0f);
            c->scale(cmd.p[2], 1.0f);
        }
        // Shaped at record time; this thread only replays glyphs.
        if (cmd.blob) c->drawTextBlob(cmd.blob, cmd.p[0], cmd.p[1], cmd.paint);
        else c->drawSimpleText(cmd.text.data(), cmd.text.size(), SkTextEncoding::kUTF8,
                               cmd.p[0], cmd.p[1], cmd.font, cmd.paint);
        if (scaled) {
            c->restore();
        }
        break;
    }
    case CanvasCmd::kDrawImage:
        c->drawImageRect(cmd.img, cmd.src, cmd.dst, cmd.samp, &cmd.paint,
                         SkCanvas::kStrict_SrcRectConstraint);
        break;
    case CanvasCmd::kPutImageData:
        c->save();
        c->resetMatrix();
        if (!cmd.src.isEmpty()) {
            c->drawImageRect(cmd.img, cmd.src, cmd.dst, cmd.samp, &cmd.paint,
                             SkCanvas::kStrict_SrcRectConstraint);
        } else {
            c->drawImage(cmd.img, cmd.p[0], cmd.p[1], cmd.samp, &cmd.paint);
        }
        c->restore();
        break;
    case CanvasCmd::kSave:    c->save(); break;
    case CanvasCmd::kRestore:
        // Never below the drawing state's own level (see ensureSurface). A
        // surface recreated by a layout resize starts without the saves that
        // were open on the old one, so a later restore can find none.
        if (c->getSaveCount() > 2) c->restore();
        break;
    case CanvasCmd::kTranslate: c->translate(cmd.p[0], cmd.p[1]); break;
    case CanvasCmd::kRotate:    c->rotate(cmd.p[0]); break;
    case CanvasCmd::kScale:     c->scale(cmd.p[0], cmd.p[1]); break;
    case CanvasCmd::kSetTransform: {
        c->resetMatrix();
        SkMatrix m;
        m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
        c->concat(m);
        break;
    }
    case CanvasCmd::kResetTransform:
        c->resetMatrix();
        break;
    case CanvasCmd::kConcatTransform: {
        SkMatrix m;
        m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
        c->concat(m);
        break;
    }
    case CanvasCmd::kReset:
        // The recorded save()s and clip()s die with the state stack, and the
        // transform goes back to the identity: restoring to the SkCanvas's
        // base level drops every one of them, including a clip made outside
        // any save(), which lives on the level ensureSurface opened above the
        // base — reopened here for the next one.
        c->restoreToCount(1);
        c->save();
        c->clear(SK_ColorTRANSPARENT);
        break;
    }
}

void CanvasScene::replayThroughFilter(SkCanvas* c, CanvasCmd& cmd, const SkM44& ctm,
                                      const sk_sp<SkImageFilter>& filter) {
    // The layer is opened under the identity matrix so the filter's lengths
    // (a blur radius, a shadow or drop-shadow offset) are canvas pixels and not
    // scaled by the current transform; the draw itself then runs under the
    // transform it was recorded with. The layer paint carries globalAlpha and
    // the composite op, which the spec applies after the filter.
    c->save();
    c->resetMatrix();
    SkPaint layer;
    layer.setImageFilter(filter);
    layer.setAlphaf(cmd.layerAlpha);
    layer.setBlendMode(cmd.layerBlend);
    c->saveLayer(nullptr, &layer);
    c->setMatrix(ctm);
    replayOne(c, cmd);
    c->restore();
    c->restore();
}

void CanvasScene::replayCommands(SkCanvas* c, std::vector<CanvasCmd>& cmds) {
    for (auto& cmd : cmds) {
        if (!cmd.filter && !cmd.shadow) {
            replayOne(c, cmd);
            continue;
        }
        const SkM44 ctm = c->getLocalToDevice();
        // The spec's drawing model: the shadow (of the filtered image, when
        // there is a filter) is composited first, with globalAlpha and the
        // composite op of its own, and then the shape is, with the same two.
        if (cmd.shadow) replayThroughFilter(c, cmd, ctm, cmd.shadow);
        if (cmd.filter) {
            replayThroughFilter(c, cmd, ctm, cmd.filter);
        } else {
            // Shadow but no filter: the shape needs no layer, so globalAlpha
            // and the composite op go back onto its own paint — exactly the
            // paint an unshadowed draw would have carried.
            const SkPaint recorded = cmd.paint;
            cmd.paint.setAlphaf(recorded.getAlphaf() * cmd.layerAlpha);
            cmd.paint.setBlendMode(cmd.layerBlend);
            replayOne(c, cmd);
            cmd.paint = recorded;
        }
    }
}

void CanvasScene::flushStagedCommands() {
    if (stagedCommands_.empty()) return;

    if (tryStreamingPutImageDataFastPath(surface_.get(), stagedCommands_)) {
        stagedCommands_.clear();
        return;
    }

    auto* c = skCanvas();
    if (!c) { stagedCommands_.clear(); return; }

    replayCommands(c, stagedCommands_);
    stagedCommands_.clear();
}

// ---------------------------------------------------------------------------
// Command buffer replay
// ---------------------------------------------------------------------------

void CanvasScene::flushCommands() {
    if (commands_.empty()) return;

    // Re-sync Skia's GL state cache: external code (engine compositing,
    // screenshot paths) may have changed FBO/program bindings since the
    // last Ganesh draw. Without this, Skia draws against stale state and
    // commands silently miss the canvas FBO. Do this BEFORE the fast path
    // too — writePixels also goes through Ganesh and benefits from a clean
    // state cache.
    if (grContext_) grContext_->resetContext();

    if (tryStreamingPutImageDataFastPath(surface_.get(), commands_)) {
        commands_.clear();
        return;
    }

    auto* c = skCanvas();
    if (!c) { commands_.clear(); return; }

    replayCommands(c, commands_);
    commands_.clear();
    // Submit GPU work so subsequent surface->readPixels (and the next
    // rasterize) see the result. Skia internally flushes on readPixels,
    // but explicit submit is needed so other GL code (engine compositing)
    // sees the canvas FBO contents.
    if (grContext_) {
        grContext_->flushAndSubmit();
    }
    dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;
}

// ---------------------------------------------------------------------------
// Compositing — upload raster pixels to GL texture
// ---------------------------------------------------------------------------

void CanvasScene::rasterize(render::GLContext* gl) {
    if (!gl) return;

    // Check if element was removed from the DOM. See note in prepareAndSignal.
    if (detachedCb_) {
        if (!backingElementAlive()) { onElementFinalized(); return; }
        bool orphaned = detachedCb_(detachedUd_);
        if (!orphaned) everAttached_ = true;
        if (orphaned && everAttached_) {
            detached_ = true;
            return;
        }
    }

    // Query element layout position (display rect). Surface size comes from
    // queryLayoutWidth/Height so intrinsic canvas.width/height beats layout.
    float layoutX = 0, layoutY = 0, layoutW = 0, layoutH = 0;
    if (layoutCb_) {
        layoutCb_(layoutUd_, layoutX, layoutY, layoutW, layoutH);
    }

    screenX_ = layoutX;
    screenY_ = layoutY - viewportScrollY_;

    int canvasW = queryLayoutWidth();
    int canvasH = queryLayoutHeight();
    if (canvasW <= 0 || canvasH <= 0) return;
    ensureSurface(canvasW, canvasH);

    flushCommands();
    dirty_ = false;
}

} // namespace bro::canvas
