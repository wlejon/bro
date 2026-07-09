#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/inspector_highlight.h"
#include "engine/overflow.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/box.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "platform/sdl_window.h"
#include "render/command_buffer.h"
#include "render/command_replayer.h"
#include "render/gl_context.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <glad/gl.h>
#include <include/gpu/ganesh/GrDirectContext.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <thread>
#include <utility>

namespace bro::engine {

void Engine::addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene) {
    if (scene) {
        scene->init(gl_.get());
        // Windowed GPU mode: bind to the shared canvas-raster worker. No GL
        // context is created here — the worker's context was created once at
        // run() start — so registering a canvas never races the raster thread.
        // (If the worker isn't up yet, e.g. a scene created during app load,
        // run()'s init binds it once the worker exists.)
        if (displayMode_ == DisplayMode::Windowed && window_) {
            if (canvasRasterThread_)
                scene->bindRasterThread(canvasRasterThread_.get());
        } else {
            // Headless / CPU fallback: use renderer's GrContext directly
            auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
            if (skia && skia->grContext()) {
                scene->setGrContext(skia->grContext());
            }
        }
    }
    if (scene) canvasSceneRegistry_[scene->sceneId()] = scene.get();
    canvasScenes_.push_back(std::move(scene));
}

void Engine::drawTexturedQuad(GLuint tex, float x, float y, float w, float h) {
    if (!tex || !gl_) return;

    render::TextureVertex quad[6] = {
        {x,   y,   0, 0}, {x+w, y,   1, 0}, {x+w, y+h, 1, 1},
        {x,   y,   0, 0}, {x+w, y+h, 1, 1}, {x,   y+h, 0, 1},
    };

    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);

    glBindVertexArray(uiQuadVAO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Engine::recordAppLayers(render::CommandBuffer& outBuffer,
                             int vpW, int vpH,
                             int insetTop, int insetRight, int insetBottom,
                             float scrollY,
                             const std::unordered_set<dom::Element*>* promotedSet,
                             bool promotedOnly) {
    if (!recordingRenderer_ || !drawTraversal_) return;

    int contentW = vpW - insetRight;
    int contentH = vpH - insetTop - insetBottom;

    // Compositor-layer paint mode. promotedSet==nullptr → the default single
    // pass (All). Base pass skips promoted subtrees (leaving holes the on-top
    // promoted layer fills); promoted pass paints ONLY those subtrees. Reset to
    // All at the end so system-panel recording and any other caller is
    // unaffected.
    using PaintMode = layout::DrawTraversal::PaintMode;
    drawTraversal_->setPromotedElements(promotedSet);
    drawTraversal_->setPaintMode(
        !promotedSet ? PaintMode::All
                     : (promotedOnly ? PaintMode::PromotedOnly
                                     : PaintMode::BaseSkipPromoted));

    outBuffer.clear();
    recordingRenderer_->setBuffer(&outBuffer);

    // Layer-break callback emits Cmd_LayerBreak. The replayer's handler does
    // the actual GPU surface management.
    drawTraversal_->setLayerBreakCallback(
        [this](canvas::CanvasScene* scene, unsigned int directTexture,
               float x, float y, float w, float h,
               float clipX, float clipY, float clipW, float clipH) {
            int kind = scene ? render::Cmd_LayerBreak::Canvas2D
                             : render::Cmd_LayerBreak::WebGL;
            recordingRenderer_->recordLayerBreak(
                kind, scene ? scene->sceneId() : 0, directTexture, x, y, w, h,
                clipX, clipY, clipW, clipH);
        });
    // <iframe> sub-documents: record a break carrying the IframeDoc id. Its
    // texture is produced by replayIframeLayers and resolved at composite time.
    drawTraversal_->setIframeLayerBreakCallback(
        [this](void* idoc, float x, float y, float w, float h,
               float clipX, float clipY, float clipW, float clipH) {
            auto* d = static_cast<IframeDoc*>(idoc);
            recordingRenderer_->recordLayerBreak(
                render::Cmd_LayerBreak::IframeDoc, d ? d->id : 0, 0, x, y, w, h,
                clipX, clipY, clipW, clipH);
        });

    // Everything below records in *content space*: the app layer surfaces are
    // content-sized (contentW × contentH) and origin-based; the engine-reserved
    // inset is applied exactly once, by the compositor, which places these
    // layers at (0, insetTop). The traversal root offset is therefore just the
    // document scroll, and lastDrawPos_ / overlay anchors / layer-break quads
    // all land in content space automatically.
    if (document_ && document_->documentElement()) {
        drawTraversal_->setBasePath(document_->basePath());
        drawTraversal_->draw(document_->documentElement(),
                             0, -scrollY,
                             contentW, contentH, /*viewportTop=*/0);

        // Selection / inspector / overlay / scrollbars are base-only chrome —
        // they belong to the cached base, never the on-top promoted layer.
        if (!promotedOnly) {
            drawSelectionHighlight(recordingRenderer_.get(), -scrollY);

            if (inspector_.visible) {
                dom::Element* highlight = inspector_.pickerMode && inspector_.pickerHover
                    ? inspector_.pickerHover
                    : inspector_.selected;
                if (highlight) {
                    drawInspectorHighlight(recordingRenderer_.get(), highlight, scrollY,
                                           /*insetLeft=*/0, /*insetTop=*/0,
                                           contentW, contentH);
                }
            }
        }
    }

    if (!promotedOnly) {
        overlayMgr_.drawIfContext(OverlayContext::App, recordingRenderer_.get());

        if (document_) {
            float vh = static_cast<float>(contentH);
            auto& vs = viewportScrollbar_.style();
            auto m = viewportScrollbar_.layout(
                static_cast<float>(contentW) - vs.width - vs.margin,
                0.0f, vh, documentHeight_, vh, scrollY);
            viewportScrollbar_.draw(recordingRenderer_.get(), m);

            drawElementScrollbars(recordingRenderer_.get(),
                                  document_->documentElement(),
                                  0.0f, -scrollY);
        }
    }

    drawTraversal_->setLayerBreakCallback(nullptr);
    drawTraversal_->setIframeLayerBreakCallback(nullptr);
    recordingRenderer_->setBuffer(nullptr);
    // Restore default paint mode so subsequent recorders (system panels, the
    // next full pass) aren't affected.
    drawTraversal_->setPaintMode(PaintMode::All);
    drawTraversal_->setPromotedElements(nullptr);
}

void Engine::replayAppLayers(render::SkiaRenderer* renderer,
                             const render::CommandBuffer& buffer,
                             std::vector<render::SkiaRenderer::GPUSurface>& pool,
                             int& poolW, int& poolH,
                             int surfW, int surfH,
                             std::vector<UILayer>& outLayers,
                             const render::CommandBuffer* promotedBuffer) {
    if (!renderer || !renderer->grContext()) return;

    // surfW/surfH are the *content* dimensions (viewport minus engine-reserved
    // insets) — app layer surfaces are content-sized. The pool compare below
    // must use these dims so a menu show/hide (contentH change) reallocates.
    if (poolW != surfW || poolH != surfH) {
        for (auto& ps : pool) renderer->destroyGPUSurface(ps);
        pool.clear();
        poolW = surfW;
        poolH = surfH;
    }
    if (pool.empty()) {
        pool.push_back(renderer->createGPUSurface(surfW, surfH));
    }
    for (auto& ps : pool) {
        renderer->rewrapGPUSurface(ps, surfW, surfH);
    }

    int htmlLayerIdx = 0;
    auto origSurface = renderer->switchSurface(pool[0].surface);

    render::CommandReplayer replayer(renderer);
    replayer.setLayerBreakHandler(
        [&](int kind, uint64_t sceneId, unsigned int directTexture,
            float x, float y, float w, float h,
            float clipX, float clipY, float clipW, float clipH) {
            int prevIdx = htmlLayerIdx;
            htmlLayerIdx++;
            while (htmlLayerIdx >= static_cast<int>(pool.size())) {
                pool.push_back(renderer->createGPUSurface(surfW, surfH));
                renderer->rewrapGPUSurface(pool.back(), surfW, surfH);
            }
            renderer->switchSurface(pool[htmlLayerIdx].surface);

            UILayer htmlLayer;
            htmlLayer.type = UILayer::HTML;
            htmlLayer.texture = pool[prevIdx].texture;
            outLayers.push_back(std::move(htmlLayer));

            UILayer quadLayer;
            quadLayer.type = (kind == render::Cmd_LayerBreak::IframeDoc)
                                 ? UILayer::Iframe : UILayer::Canvas;
            quadLayer.canvasSceneId = sceneId;    // CanvasScene id or IframeDoc id
            quadLayer.texture = directTexture;     // WebGL direct texture (0 otherwise)
            quadLayer.cx = x; quadLayer.cy = y;
            quadLayer.cw = w; quadLayer.ch = h;
            quadLayer.clipX = clipX; quadLayer.clipY = clipY;
            quadLayer.clipW = clipW; quadLayer.clipH = clipH;
            outLayers.push_back(std::move(quadLayer));
        });

    replayer.replay(buffer);

    // Capture the trailing HTML layer.
    renderer->switchSurface(origSurface);
    UILayer lastHtml;
    lastHtml.type = UILayer::HTML;
    lastHtml.texture = pool[htmlLayerIdx].texture;
    outLayers.push_back(std::move(lastHtml));

    // Compositor-promoted layer: replay the promoted subtrees into one extra
    // pool surface and append it as the topmost HTML layer, filling the holes
    // the base pass left. Painted in content space at absolute offsets (same
    // walk as the base), so a full-surface quad lines up 1:1.
    int lastIdx = htmlLayerIdx;
    if (promotedBuffer && promotedBuffer->commandCount() > 0) {
        int promotedIdx = htmlLayerIdx + 1;
        while (promotedIdx >= static_cast<int>(pool.size())) {
            pool.push_back(renderer->createGPUSurface(surfW, surfH));
            renderer->rewrapGPUSurface(pool.back(), surfW, surfH);
        }
        renderer->switchSurface(pool[promotedIdx].surface);
        render::CommandReplayer promotedReplayer(renderer);
        // A canvas/WebGL element inside a promoted subtree would emit a break;
        // capability-1 promoted layers are plain CSS subtrees, so swallow any
        // break (no-op) rather than risk a null-handler call. Refined later.
        promotedReplayer.setLayerBreakHandler(
            [](int, uint64_t, unsigned int, float, float, float, float,
               float, float, float, float) {});
        promotedReplayer.replay(*promotedBuffer);
        renderer->switchSurface(origSurface);

        UILayer promotedLayer;
        promotedLayer.type = UILayer::HTML;
        promotedLayer.texture = pool[promotedIdx].texture;
        outLayers.push_back(std::move(promotedLayer));
        lastIdx = promotedIdx;
    }

    // Flush each pool surface's deferred Ganesh ops.
    for (int i = 0; i <= lastIdx; ++i) {
        if (pool[i].surface && renderer->grContext()) {
            renderer->grContext()->flush(pool[i].surface.get());
        }
    }
}

void Engine::recordSystemPanelLayers(render::CommandBuffer& outBuffer,
                                     int vpW, int vpH) {
    outBuffer.clear();
    if (!recordingRenderer_ || !drawTraversal_ || !isSystemVisible()) return;

    // Layout still runs on main thread using textMetrics_ (paired with
    // renderer_, which the recording renderer also delegates to).
    layoutSystemPanels(*textMetrics_);

    recordingRenderer_->setBuffer(&outBuffer);

    bool first = true;
    for (auto& sdoc : systemDocs_) {
        if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;

        // Between panels, emit an HtmlSurface boundary so the replayer
        // captures the current panel into a UILayer and starts a new surface.
        if (!first) {
            recordingRenderer_->recordLayerBreak(
                render::Cmd_LayerBreak::HtmlSurface, 0, 0, 0, 0, 0, 0);
        }
        first = false;

        drawSystemPanelDoc(recordingRenderer_.get(), *drawTraversal_, sdoc, vpW, vpH);
    }

    recordingRenderer_->setBuffer(nullptr);
    systemDirty_ = false;
}

void Engine::replaySystemPanelLayers(render::SkiaRenderer* renderer,
                                     const render::CommandBuffer& buffer,
                                     std::vector<render::SkiaRenderer::GPUSurface>& pool,
                                     int& poolW, int& poolH,
                                     int vpW, int vpH,
                                     std::vector<UILayer>& outLayers) {
    if (!renderer || !renderer->grContext()) return;
    if (buffer.commandCount() == 0) return;

    if (poolW != vpW || poolH != vpH) {
        for (auto& ps : pool) renderer->destroyGPUSurface(ps);
        pool.clear();
        poolW = vpW;
        poolH = vpH;
    }

    auto ensurePoolAt = [&](size_t idx) {
        while (idx >= pool.size()) {
            pool.push_back(renderer->createGPUSurface(vpW, vpH));
            renderer->rewrapGPUSurface(pool.back(), vpW, vpH);
        }
    };

    size_t panelIdx = 0;
    ensurePoolAt(panelIdx);
    renderer->rewrapGPUSurface(pool[panelIdx], vpW, vpH);
    auto origSurface = renderer->switchSurface(pool[panelIdx].surface);

    auto* grCtx = renderer->grContext();

    render::CommandReplayer replayer(renderer);
    replayer.setLayerBreakHandler(
        [&](int kind, uint64_t /*sceneId*/, unsigned int /*tex*/,
            float, float, float, float, float, float, float, float) {
            if (kind != render::Cmd_LayerBreak::HtmlSurface) return;
            // Capture current panel into a UILayer, advance to next surface.
            if (grCtx) grCtx->flush(pool[panelIdx].surface.get());
            UILayer panelLayer;
            panelLayer.type = UILayer::HTML;
            panelLayer.texture = pool[panelIdx].texture;
            outLayers.push_back(std::move(panelLayer));

            panelIdx++;
            ensurePoolAt(panelIdx);
            renderer->rewrapGPUSurface(pool[panelIdx], vpW, vpH);
            renderer->switchSurface(pool[panelIdx].surface);
        });
    replayer.setBlitCanvasInlineHandler(
        [&](void* scenePtr, float x, float y, float w, float h) {
            auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
            if (!scene || w <= 0 || h <= 0) return;
            if (grCtx) scene->setGrContext(grCtx);
            scene->flushStaged();
            auto* src = scene->surface();
            if (!src) return;
            auto img = src->makeImageSnapshot();
            if (!img) return;
            auto* c = renderer->getCanvas();
            if (!c) return;
            SkRect dst = SkRect::MakeXYWH(x, y, w, h);
            c->drawImageRect(img, dst, SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });

    replayer.replay(buffer);

    // Capture the final panel.
    if (grCtx) grCtx->flush(pool[panelIdx].surface.get());
    UILayer panelLayer;
    panelLayer.type = UILayer::HTML;
    panelLayer.texture = pool[panelIdx].texture;
    outLayers.push_back(std::move(panelLayer));

    renderer->switchSurface(origSurface);
}

// Main thread: record each iframe sub-document's paint into its own command
// buffer. Its own <canvas>es blit inline into the iframe surface (they are not
// app-level canvas layers). Called after recordAppLayers so the app's layer-
// break callbacks are already cleared and won't fire on the sub-doc traversal.
void Engine::recordIframeLayers() {
    if (iframeDocs_.empty() || !recordingRenderer_ || !drawTraversal_) return;
    for (auto& d : iframeDocs_) {
        d->cmdBuffer.clear();
        if (!d->document || !d->document->documentElement()) continue;
        // Refresh the box from the host <iframe> element's current layout, so a
        // resized preview re-lays-out (and re-rasterizes) at the new size instead
        // of stretching a stale-size texture. The element was laid out by the
        // host pass earlier this frame; contentRect is current here.
        if (d->element) {
            auto& lb = d->element->layoutBox();
            if (lb.contentRect.width  > 0) d->boxW = static_cast<int>(lb.contentRect.width);
            if (lb.contentRect.height > 0) d->boxH = static_cast<int>(lb.contentRect.height);
        }
        // Style + lay the sub-document out at its box (mirrors layoutSystemPanels):
        // JS may have mutated its DOM since the last frame, and the text runs the
        // draw consumes are produced here.
        // Point the style adapter's hover/active state at THIS sub-document's
        // targets before resolving it (the thread-local was last set for the
        // host doc). :hover in the sub-doc resolves against its own hovered
        // element; restored to the host's on the next host resolveStyles.
        layout::ElementRefAdapter::setHoveredElement(d->hoveredElement);
        d->document->resolveStyles();
        d->document->performLayout(static_cast<float>(d->boxW),
                                   static_cast<float>(d->boxH), *textMetrics_);
        // Hand this frame's <canvas> draw calls (recorded into each scene's
        // command list by JS on the main thread) to the scene's staged buffer,
        // so the raster-side inline blit (replayIframeLayers) can replay them.
        // Mirrors stageSystemPanelCanvases for panels.
        for (auto& scene : d->canvasScenes) {
            if (scene) scene->stageCommandsForRaster();
        }
        recordingRenderer_->setBuffer(&d->cmdBuffer);
        drawTraversal_->setLayerBreakCallback(
            [this](canvas::CanvasScene* scene, unsigned int, float x, float y,
                   float w, float h, float, float, float, float) {
                if (scene) recordingRenderer_->recordBlitCanvasInline(scene, x, y, w, h);
            });
        drawTraversal_->setBasePath(d->document->basePath());
        drawTraversal_->draw(d->document->documentElement(), 0, 0,
                             static_cast<float>(d->boxW),
                             static_cast<float>(d->boxH), /*viewportTop=*/0);
        drawTraversal_->setLayerBreakCallback(nullptr);
        recordingRenderer_->setBuffer(nullptr);
    }
}

// Raster thread: replay each iframe sub-document's command buffer into a box-
// sized GPU surface, and stash the resulting texture on the IframeDoc for the
// app compositor to draw at the <iframe> element's box.
void Engine::replayIframeLayers(render::SkiaRenderer* renderer) {
    if (!renderer || !renderer->grContext() || iframeDocs_.empty()) return;
    auto* grCtx = renderer->grContext();
    sk_sp<SkSurface> origSurface;
    bool haveOrig = false;
    for (auto& d : iframeDocs_) {
        if (d->cmdBuffer.commandCount() == 0) { d->fboTexture = 0; continue; }
        int bw = std::max(1, d->boxW), bh = std::max(1, d->boxH);
        if (!d->surface.surface || d->surfW != bw || d->surfH != bh) {
            if (d->surface.surface) renderer->destroyGPUSurface(d->surface);
            d->surface = renderer->createGPUSurface(bw, bh);
            d->surfW = bw; d->surfH = bh;
        }
        renderer->rewrapGPUSurface(d->surface, bw, bh);
        auto prev = renderer->switchSurface(d->surface.surface);
        if (!haveOrig) { origSurface = prev; haveOrig = true; }
        if (auto* c = renderer->getCanvas()) c->clear(SK_ColorTRANSPARENT);

        render::CommandReplayer replayer(renderer);
        replayer.setBlitCanvasInlineHandler(
            [&](void* scenePtr, float x, float y, float w, float h) {
                auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
                if (!scene || w <= 0 || h <= 0) return;
                if (grCtx) scene->setGrContext(grCtx);
                scene->flushStaged();
                auto* src = scene->surface();
                if (!src) return;
                auto img = src->makeImageSnapshot();
                if (!img) return;
                auto* c = renderer->getCanvas();
                if (!c) return;
                if (grCtx) grCtx->resetContext(); // canvas ensureSurface did raw GL; resync before sampling
                SkRect dst = SkRect::MakeXYWH(x, y, w, h);
                c->drawImageRect(img, dst, SkSamplingOptions(SkFilterMode::kLinear));
                scene->clearDirty();
            });
        replayer.replay(d->cmdBuffer);
        if (grCtx) grCtx->flush(d->surface.surface.get());
        d->fboTexture = d->surface.texture;
    }
    if (haveOrig) renderer->switchSurface(origSurface);
}

// Authoritative, synchronous capture of an <iframe> sub-document's pixels for
// iframe.capture() (the maker-agent's "look"). Rather than sampling whatever the
// async raster thread last produced into fboTexture — which lags a reload() by a
// frame or two, so the first look after a write returns the OLD view — this
// brings the sub-doc fully current on the calling (main) thread: quiesce the
// raster worker, apply any queued reload(), re-record at the element's CURRENT
// box, and render with the main-thread Skia renderer into a throwaway surface.
// Result: look()==reload()+capture() returns the just-written app on the FIRST
// call, and a resized preview is captured at its new size — no rAF timing games.
std::vector<uint8_t> Engine::captureIframe(dom::Element* el, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!el || !gl_) return {};

    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia || !skia->grContext() || !recordingRenderer_ || !drawTraversal_) {
        // No main-thread GPU Skia (e.g. --no-gpu CPU renderer): fall back to the
        // last raster-produced texture, if any.
        IframeDoc* d = iframeDocForElement(el);
        if (!d || d->fboTexture == 0 || d->surfW <= 0 || d->surfH <= 0) return {};
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, d->fboTexture, 0);
        std::vector<uint8_t> px(static_cast<size_t>(d->surfW) * d->surfH * 4);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            glReadPixels(0, 0, d->surfW, d->surfH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        else
            px.clear();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        if (px.empty()) return {};
        outW = d->surfW;
        outH = d->surfH;
        return px;
    }

    // Wait until the raster worker is neither Requested nor Busy, so it is not
    // mid-replay reading iframeDocs_ / cmdBuffer while we mutate them below. We
    // deliberately do NOT claim a ResultReady fence — that belongs to the frame
    // loop's consumeIfReady(); a finished-but-unclaimed worker has already
    // stopped touching our state, which is all we need.
    if (framePresenter_)
        while (framePresenter_->isRasterBusyOrRequested())
            std::this_thread::yield();

    processPendingIframeReloads();
    recordIframeLayers();

    IframeDoc* d = iframeDocForElement(el);
    if (!d || d->cmdBuffer.commandCount() == 0) return {};
    int w = std::max(1, d->boxW), h = std::max(1, d->boxH);

    auto* grCtx = skia->grContext();
    grCtx->resetContext();
    render::SkiaRenderer::GPUSurface surf = skia->createGPUSurface(w, h);
    if (!surf.surface) { grCtx->resetContext(); return {}; }
    auto prev = skia->switchSurface(surf.surface);
    if (auto* c = skia->getCanvas()) c->clear(SK_ColorTRANSPARENT);

    render::CommandReplayer replayer(skia);
    replayer.setBlitCanvasInlineHandler(
        [&](void* scenePtr, float x, float y, float ww, float hh) {
            auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
            if (!scene || ww <= 0 || hh <= 0) return;
            scene->setGrContext(grCtx);
            scene->flushStaged();
            auto* src = scene->surface();
            if (!src) return;
            auto img = src->makeImageSnapshot();
            if (!img) return;
            auto* c = skia->getCanvas();
            if (!c) return;
            grCtx->resetContext();  // canvas ensureSurface did raw GL; resync
            c->drawImageRect(img, SkRect::MakeXYWH(x, y, ww, hh),
                             SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });
    replayer.replay(d->cmdBuffer);
    grCtx->flush(surf.surface.get());

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, surf.texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        pixels.clear();
    else
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);

    skia->switchSurface(prev);
    skia->destroyGPUSurface(surf);
    grCtx->resetContext();

    if (pixels.empty()) return {};
    outW = w;
    outH = h;
    return pixels;
}

void Engine::compositeLayers(const std::vector<UILayer>& layers, GLuint targetFBO,
                             int offsetY, int layerW, int layerH) {
    if (!gl_) return;
    if (layers.empty()) return;

    float vw = static_cast<float>(viewportWidth_);
    float vh = static_cast<float>(viewportHeight_);

    // Placement of this layer set. App layers are content-sized and recorded
    // in content space: their HTML quads composite at (0, offsetY) with
    // content dimensions (full texture UV), and canvas/WebGL/scene quads +
    // scissor clips (recorded in content space) get offsetY added here — the
    // single place the engine-reserved top inset enters the composite.
    // System-panel layer sets keep full-viewport placement at (0, 0)
    // (offsetY = 0, layerW/H < 0).
    float lw = layerW >= 0 ? static_cast<float>(layerW) : vw;
    float lh = layerH >= 0 ? static_cast<float>(layerH) : vh;
    float oy = static_cast<float>(offsetY);

    glBindFramebuffer(GL_FRAMEBUFFER, targetFBO);
    glViewport(0, 0, viewportWidth_, viewportHeight_);

    glUseProgram(gl_->textureProgram());
    float viewport[2] = {vw, vh};
    glUniform2fv(gl_->textureViewportLoc(), 1, viewport);
    glUniform1i(gl_->textureSamplerLoc(), 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    // Bind VAO and set up vertex attribs once for all quads
    glBindVertexArray(uiQuadVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, uiQuadVBO_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::TextureVertex), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                          sizeof(render::TextureVertex),
                          (void*)offsetof(render::TextureVertex, u));
    glActiveTexture(GL_TEXTURE0);

    for (auto& layer : layers) {
        if (layer.type == UILayer::HTML) {
            if (layer.texture) {
                render::TextureVertex quad[6] = {
                    {0,  oy,    0, 0}, {lw, oy,    1, 0}, {lw, oy+lh, 1, 1},
                    {0,  oy,    0, 0}, {lw, oy+lh, 1, 1}, {0,  oy+lh, 0, 1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, layer.texture);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        } else if (layer.type == UILayer::Iframe) {
            // Iframe sub-document layer — texture resolved through the iframe
            // registry (null if the sub-document was torn down since recording).
            // The sub-doc renders into a top-down Skia GPU surface, so V is
            // oriented like a Canvas2D layer (0 at top).
            GLuint tex = 0;
            if (auto* d = iframeDocById(layer.canvasSceneId)) tex = d->fboTexture;
            if (tex) {
                float cx = layer.cx, cy = layer.cy + oy;
                float cw = layer.cw, ch = layer.ch;
                bool scissored = false;
                if (layer.clipW >= 0.0f && layer.clipH >= 0.0f) {
                    float clipY = layer.clipY + oy;
                    int sx = static_cast<int>(std::floor(layer.clipX));
                    int sw = static_cast<int>(std::ceil(layer.clipX + layer.clipW)) - sx;
                    int syTop = static_cast<int>(std::floor(clipY));
                    int sh = static_cast<int>(std::ceil(clipY + layer.clipH)) - syTop;
                    int sy = viewportHeight_ - (syTop + sh);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(sx, sy, std::max(0, sw), std::max(0, sh));
                    scissored = true;
                }
                render::TextureVertex quad[6] = {
                    {cx,    cy,    0, 0}, {cx+cw, cy,    1, 0}, {cx+cw, cy+ch, 1, 1},
                    {cx,    cy,    0, 0}, {cx+cw, cy+ch, 1, 1}, {cx,    cy+ch, 0, 1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, tex);
                glDrawArrays(GL_TRIANGLES, 0, 6);
                if (scissored) glDisable(GL_SCISSOR_TEST);
            }
        } else {
            // Canvas/WebGL layer — get texture from canvas scene or direct
            // texture. A canvasSceneId that no longer resolves (scene detached
            // since this layer was recorded) draws nothing this frame.
            GLuint tex = 0;
            bool isCanvas = layer.canvasSceneId != 0;
            if (isCanvas) {
                if (auto* cs = canvasSceneById(layer.canvasSceneId))
                    tex = cs->texture();
            } else {
                tex = layer.texture;  // WebGL direct texture
            }
            if (tex) {
                float cx = layer.cx, cy = layer.cy + oy;
                float cw = layer.cw, ch = layer.ch;

                // WebGL textures are bottom-up (origin at lower-left) so flip V coords
                float v0 = isCanvas ? 0.0f : 1.0f;
                float v1 = isCanvas ? 1.0f : 0.0f;

                // Canvas/WebGL layers composite outside the Skia clip stack, so
                // re-apply any ancestor overflow/scroll clip as a GL scissor.
                // Clip space is the layer set's recorded space (content space
                // for app layers — add oy, like the quad); scissor is
                // bottom-left window coords, so flip Y against the full
                // viewport height. clipW < 0 ⇒ unclipped.
                bool scissored = false;
                if (layer.clipW >= 0.0f && layer.clipH >= 0.0f) {
                    float clipY = layer.clipY + oy;
                    int sx = static_cast<int>(std::floor(layer.clipX));
                    int sw = static_cast<int>(std::ceil(layer.clipX + layer.clipW)) - sx;
                    int syTop = static_cast<int>(std::floor(clipY));
                    int sh = static_cast<int>(std::ceil(clipY + layer.clipH)) - syTop;
                    int sy = viewportHeight_ - (syTop + sh);
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(sx, sy, std::max(0, sw), std::max(0, sh));
                    scissored = true;
                }

                render::TextureVertex quad[6] = {
                    {cx,    cy,    0, v0}, {cx+cw, cy,    1, v0}, {cx+cw, cy+ch, 1, v1},
                    {cx,    cy,    0, v0}, {cx+cw, cy+ch, 1, v1}, {cx,    cy+ch, 0, v1},
                };
                glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_DYNAMIC_DRAW);
                glBindTexture(GL_TEXTURE_2D, tex);
                glDrawArrays(GL_TRIANGLES, 0, 6);

                if (scissored) glDisable(GL_SCISSOR_TEST);
            }
        }
    }
}

} // namespace bro::engine
