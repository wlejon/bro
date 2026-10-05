#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/sub_document.h"
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
#include "render/recording_renderer.h"
#include "render/pixel_convert.h"
#include "render/skia_backend.h"
#include "webgl/webgl2_context.h"

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <thread>
#include <utility>

namespace bro::engine {

void Engine::addCanvasScene(std::unique_ptr<canvas::CanvasScene> scene) {
    if (!scene) return;
    scene->init();
    canvasSceneRegistry_[scene->sceneId()] = scene.get();
    canvasScenes_.push_back(std::move(scene));
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
    // the actual layer surface management.
    drawTraversal_->setLayerBreakCallback(
        [this](int kind, canvas::CanvasScene* scene, unsigned int elementId,
               float x, float y, float w, float h,
               float clipX, float clipY, float clipW, float clipH) {
            recordingRenderer_->recordLayerBreak(
                kind, scene ? scene->sceneId() : 0, elementId, x, y, w, h,
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
            // The viewport scrollbar belongs to the root element, whose
            // color-scheme (and scrollbar-color) themes it.
            auto* rootEl = document_->documentElement();
            viewportScrollbar_.draw(recordingRenderer_.get(), m,
                rootEl ? Scrollbar::colorsFor(rootEl->computedStyle(),
                             document_->mediaContext().colorScheme == "dark")
                       : Scrollbar::schemeColors(
                             document_->mediaContext().colorScheme == "dark"));

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
                             std::vector<render::SkiaRenderer::LayerSurface>& pool,
                             int& poolW, int& poolH,
                             int surfW, int surfH,
                             std::vector<UILayer>& outLayers,
                             const render::CommandBuffer* promotedBuffer) {
    if (!renderer) return;

    // surfW/surfH are the *content* dimensions (viewport minus engine-reserved
    // insets) — app layer surfaces are content-sized. The pool compare below
    // must use these dims so a menu show/hide (contentH change) reallocates.
    if (poolW != surfW || poolH != surfH) {
        for (auto& ps : pool) renderer->releaseLayerSurface(ps);
        pool.clear();
        poolW = surfW;
        poolH = surfH;
    }
    if (pool.empty()) {
        pool.push_back(renderer->createLayerSurface(surfW, surfH));
    }

    int htmlLayerIdx = 0;
    auto origSurface = renderer->switchSurface(pool[0].surface);

    render::CommandReplayer replayer(renderer);
    replayer.setLayerBreakHandler(
        [&](int kind, uint64_t sceneId, unsigned int elementId,
            float x, float y, float w, float h,
            float clipX, float clipY, float clipW, float clipH) {
            int prevIdx = htmlLayerIdx;
            htmlLayerIdx++;
            while (htmlLayerIdx >= static_cast<int>(pool.size())) {
                pool.push_back(renderer->createLayerSurface(surfW, surfH));
            }
            renderer->switchSurface(pool[htmlLayerIdx].surface);

            UILayer htmlLayer;
            htmlLayer.type = UILayer::HTML;
            htmlLayer.surface = pool[prevIdx].surface;
            outLayers.push_back(std::move(htmlLayer));

            UILayer quadLayer;
            if (kind == render::Cmd_LayerBreak::IframeDoc) {
                quadLayer.type = UILayer::Iframe;
            } else if (kind == render::Cmd_LayerBreak::Scene3D) {
                quadLayer.type = UILayer::Scene3D;
            } else if (kind == render::Cmd_LayerBreak::WebGL) {
                quadLayer.type = UILayer::WebGL;
            } else {
                quadLayer.type = UILayer::Canvas;
            }
            quadLayer.canvasSceneId = sceneId;    // CanvasScene id or IframeDoc id
            quadLayer.elementId = elementId;      // the WebGL/Scene3D element (0 otherwise)
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
    lastHtml.surface = pool[htmlLayerIdx].surface;
    outLayers.push_back(std::move(lastHtml));

    // Compositor-promoted layer: replay the promoted subtrees into one extra
    // pool surface and append it as the topmost HTML layer, filling the holes
    // the base pass left. Painted in content space at absolute offsets (same
    // walk as the base), so a full-surface quad lines up 1:1.
    if (promotedBuffer && promotedBuffer->commandCount() > 0) {
        int promotedIdx = htmlLayerIdx + 1;
        while (promotedIdx >= static_cast<int>(pool.size())) {
            pool.push_back(renderer->createLayerSurface(surfW, surfH));
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
        promotedLayer.surface = pool[promotedIdx].surface;
        outLayers.push_back(std::move(promotedLayer));
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
                                     std::vector<render::SkiaRenderer::LayerSurface>& pool,
                                     int& poolW, int& poolH,
                                     int vpW, int vpH,
                                     std::vector<UILayer>& outLayers) {
    if (!renderer) return;
    if (buffer.commandCount() == 0) return;

    if (poolW != vpW || poolH != vpH) {
        for (auto& ps : pool) renderer->releaseLayerSurface(ps);
        pool.clear();
        poolW = vpW;
        poolH = vpH;
    }

    auto ensurePoolAt = [&](size_t idx) {
        while (idx >= pool.size()) {
            pool.push_back(renderer->createLayerSurface(vpW, vpH));
        }
    };

    size_t panelIdx = 0;
    ensurePoolAt(panelIdx);
    renderer->fitLayerSurface(pool[panelIdx], vpW, vpH);
    auto origSurface = renderer->switchSurface(pool[panelIdx].surface);

    render::CommandReplayer replayer(renderer);
    replayer.setLayerBreakHandler(
        [&](int kind, uint64_t /*sceneId*/, unsigned int /*tex*/,
            float, float, float, float, float, float, float, float) {
            if (kind != render::Cmd_LayerBreak::HtmlSurface) return;
            // Capture current panel into a UILayer, advance to next surface.
            UILayer panelLayer;
            panelLayer.type = UILayer::HTML;
            panelLayer.surface = pool[panelIdx].surface;
            outLayers.push_back(std::move(panelLayer));

            panelIdx++;
            ensurePoolAt(panelIdx);
            renderer->fitLayerSurface(pool[panelIdx], vpW, vpH);
            renderer->switchSurface(pool[panelIdx].surface);
        });
    replayer.setBlitCanvasInlineHandler(
        [&](void* scenePtr, float x, float y, float w, float h) {
            auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
            if (!scene || w <= 0 || h <= 0) return;
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
    UILayer panelLayer;
    panelLayer.type = UILayer::HTML;
    panelLayer.surface = pool[panelIdx].surface;
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
        // Refresh the box from the host <iframe> element's current layout, so a
        // resized preview re-lays-out (and re-rasterizes) at the new size instead
        // of stretching a stale-size texture. The element was laid out by the
        // host pass earlier this frame; contentRect is current here. Goes
        // through syncIframeBox so the media viewport moves with the box too —
        // this used to set boxW/boxH directly and leave @media / matchMedia
        // pinned to the creation-time size.
        syncIframeBox(*d);
        recordSubDoc(iframeSubDoc(*d), recordingRenderer_.get(), drawTraversal_.get(),
                     *textMetrics_);
    }
}

// Raster thread: replay each iframe sub-document's command buffer into a box-
// sized layer surface, and publish a snapshot of it on the IframeDoc for the
// app compositor to draw at the <iframe> element's box.
void Engine::replayIframeLayers(render::SkiaRenderer* renderer) {
    if (!renderer) return;
    // Whoever replays the sub-docs OWNS their surfaces — the raster thread
    // windowed, the main thread headless (screenshot() replays inline, there
    // being no raster thread). So this is exactly the right place to release the
    // ones orphaned since the last replay: nothing else can be drawing into
    // them. Ahead of the empty check, or churn that removed
    // the last iframe would leave its surface queued forever.
    drainIframeSurfaceFrees(renderer);
    for (auto& d : iframeDocs_) replaySubDoc(iframeSubDoc(*d), renderer);
}

// Main thread: record each secondary window host's document, exactly like an
// iframe sub-document but sized to the OS window's client area rather than an
// element box. Runs in the same raster-idle record block.
void Engine::recordWindowHostLayers() {
    if (windowHosts_.empty() || !recordingRenderer_ || !drawTraversal_) return;
    for (auto& h : windowHosts_) {
        if (!h->document || h->pendingClose) continue;
        // A minimized host is not presented, so there is nothing to record —
        // but keep the last recording (and its texture) so a restore shows the
        // old frame until the next one paints, rather than flashing blank.
        if (h->minimized) continue;
        if (h->window && displayMode_ == DisplayMode::Windowed)
            h->renderScale = h->window->getPixelDensity();
        syncWindowHostBox(*h);
        recordSubDoc(windowHostSubDoc(*h), recordingRenderer_.get(),
                     drawTraversal_.get(), *textMetrics_);
    }
}

// Raster thread: replay each host document into its window-sized surface ->
// WindowHost::published, which compositeWindowHosts() presents on that host's
// window.
void Engine::replayWindowHostLayers(render::SkiaRenderer* renderer) {
    if (!renderer) return;
    const float appScale = renderer->deviceScale();
    for (auto& h : windowHosts_) {
        if (h->pendingClose) continue;
        renderer->setDeviceScale(h->renderScale);
        replaySubDoc(windowHostSubDoc(*h), renderer);
    }
    renderer->setDeviceScale(appScale);
}

// Authoritative, synchronous capture of an <iframe> sub-document's pixels for
// iframe.capture() (the maker-agent's "look"). Rather than sampling whatever the
// async raster thread last published — which lags a reload() by a
// frame or two, so the first look after a write returns the OLD view — this
// brings the sub-doc fully current on the calling (main) thread: quiesce the
// raster worker, apply any queued reload(), re-record at the element's CURRENT
// box, and render with the main-thread Skia renderer into a throwaway surface.
// Result: look()==reload()+capture() returns the just-written app on the FIRST
// call, and a resized preview is captured at its new size — no rAF timing games.
std::vector<uint8_t> Engine::captureIframe(dom::Element* el, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!el) return {};

    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia || !recordingRenderer_ || !drawTraversal_) {
        // No main-thread Skia renderer (e.g. --no-gpu CPU renderer): fall back
        // to the frame the raster thread last published, if any.
        IframeDoc* d = iframeDocForElement(el);
        if (!d) return {};
        return readPublishedFrame(d->published, outW, outH);
    }

    quiesceRasterForCapture();
    processPendingIframeReloads();
    recordIframeLayers();

    IframeDoc* d = iframeDocForElement(el);
    if (!d) return {};
    return captureSubDoc(iframeSubDoc(*d), skia, outW, outH);
}

// The same "look" for a secondary window host, keyed by host id: quiesce the
// raster worker, re-record the host document at its CURRENT window size, and
// render it on the main thread. Used by the parent-side handle's capture(),
// which is how a headless test observes a secondary window at all.
std::vector<uint8_t> Engine::captureWindowHost(uint64_t id, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    WindowHost* h = windowHostById(id);
    if (!h || !h->document) return {};

    auto* skia = dynamic_cast<render::SkiaRenderer*>(renderer_.get());
    if (!skia || !recordingRenderer_ || !drawTraversal_)
        return readPublishedFrame(h->published, outW, outH);

    quiesceRasterForCapture();
    syncWindowHostBox(*h);
    recordSubDoc(windowHostSubDoc(*h), recordingRenderer_.get(),
                 drawTraversal_.get(), *textMetrics_);
    return captureSubDoc(windowHostSubDoc(*h), skia, outW, outH);
}

// Wait until the raster worker is neither Requested nor Busy, so it is not
// mid-replay reading the sub-doc registries / command buffers while a capture
// mutates them. We deliberately do NOT claim a ResultReady fence — that belongs
// to the frame loop's consumeIfReady(); a finished-but-unclaimed worker has
// already stopped touching our state, which is all we need.
void Engine::quiesceRasterForCapture() {
    if (!framePresenter_) return;
    while (framePresenter_->isRasterBusyOrRequested())
        std::this_thread::yield();
}

// Fallback shared by both capture paths: the frame the last replay published,
// with no re-record.
std::vector<uint8_t> Engine::readPublishedFrame(const PublishedFrame& frame, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    sk_sp<SkImage> img = frame.get();
    SkPixmap pixmap;
    if (!img || !img->peekPixels(&pixmap)) return {};
    auto pixels = render::pixmapToRgba(pixmap);
    if (pixels.empty()) return {};
    outW = pixmap.width();
    outH = pixmap.height();
    return pixels;
}

} // namespace bro::engine
