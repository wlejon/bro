#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/sub_document.h"
#include "engine/terminal_layers.h"
#include "engine/inspector_highlight.h"
#include "engine/overflow.h"

#include "canvas/canvas_scene.h"
#include "dom/document.h"
#include "dom/element.h"
#include "layout/box.h"
#include "layout/draw_traversal.h"
#include "layout/element_ref_adapter.h"
#include "layout/skia_text_metrics.h"
#include "platform/window.h"
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
    canvasSceneRegistry_[scene->sceneId()] = scene.get();
    canvasScenes_.push_back(std::move(scene));
}

void Engine::recordAppLayers(render::CommandBuffer& outBuffer,
                             int vpW, int vpH,
                             int insetTop, int insetRight, int insetBottom,
                             float scrollY,
                             const std::unordered_set<dom::Element*>* promotedSet,
                             bool promotedOnly, ScrollLayerSet* scrollLayers) {
    if (!recordingRenderer_ || !drawTraversal_) return;
    // Scroll layers for this pass (with the paint cache: off, a pass is the
    // plain whole record a test compares against). Every layer the pass does
    // not record again is dropped at its end.
    if (scrollLayers && !layout::DrawTraversal::paintCacheEnabled()) scrollLayers = nullptr;
    if (scrollLayers) scrollLayers->beginPass();
    drawTraversal_->setScrollLayerHost(scrollLayers);
    // Overflow clips on whole device pixels of the app's surfaces.
    drawTraversal_->setSnapScale(deviceScale_.render);

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

    // A layer break records Cmd_LayerBreak; the replayer's handler splits
    // the HTML surfaces around it.
    drawTraversal_->setLayerBreakCallback([this](const layout::DrawTraversal::LayerBreak& lb) {
        recordingRenderer_->recordLayerBreak(lb.source, lb.quad);
    });
    drawTraversal_->setTerminalLayers(!promotedOnly && terminalLayersEnabled());
    // A shell host's client windows go where its desktop level ends,
    // interleaved with the frames it draws for them (window_frames.h).
    bool hasCompositor = false;
#if BRO_WITH_COMPOSITOR
    hasCompositor = isShellApp() && drmCtx_ && drmCtx_->compositor;
#endif
    const bool clientWindows = !promotedOnly && hasCompositor;
    drawTraversal_->setShellClientWindowsLayer(clientWindows);
    std::vector<layout::DrawTraversal::ClientWindowSlot> clientSlots;
    if (clientWindows) {
        clientSlots = drmCtx_->frames.slots();
        uint32_t listId = 0;
        auto& refs = drmCtx_->frames.beginList(listId);
        drawTraversal_->setClientWindows(&clientSlots, &refs, listId);
    }

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
                                  0.0f, -scrollY, deviceScale_.render, scrollLayers, recordingRenderer_.get());
        }
    }

    drawTraversal_->setLayerBreakCallback(nullptr);
    drawTraversal_->setTerminalLayers(false);
    drawTraversal_->setShellClientWindowsLayer(false);
    drawTraversal_->setClientWindows(nullptr, nullptr, 0);
    recordingRenderer_->setBuffer(nullptr);
    // Restore default paint mode so subsequent recorders (system panels, the
    // next full pass) aren't affected.
    drawTraversal_->setPaintMode(PaintMode::All);
    drawTraversal_->setPromotedElements(nullptr);
    drawTraversal_->setScrollLayerHost(nullptr);
    drawTraversal_->setSnapScale(0.0f);
    if (scrollLayers) scrollLayers->endPass();
}

bool Engine::absorbLayerScroll(dom::Element* e, float scrollLeft, float scrollTop) {
    if (!e || !document_ || e->document() != document_.get() || !baseValid_ ||
        !layout::DrawTraversal::paintCacheEnabled())
        return false;
    // What the base paints where a scroll inside it puts it: a selection
    // highlight or caret, the inspector's highlight, an overlay.
    const auto& sel = selectionSnapshot_;
    if (!sel.rects.empty() || sel.hasCaret || !sel.compUnderlines.empty()) return false;
    if (inspector_.visible || overlayMgr_.hasActive()) return false;
    bool rasterDue = false;
    if (!baseScrollLayers_.scrollTo(e, scrollLeft, scrollTop, deviceScale_.render, &rasterDue)) return false;
    if (rasterDue) uiDirty_ = true;  // a raster pass, no record: the layer at its new phase
    return true;
}

namespace {

// The UILayer for an HTML layer surface: its GPU image, or the CPU surface.
UILayer htmlLayerOf(const render::LayerSurface& s) {
    HtmlLayer html;
    if (s.isGpu()) html.image = s.image;
    else html.surface = s.surface;
    UILayer layer;
    layer.content = std::move(html);
    return layer;
}

} // namespace

render::Damage Engine::recordDamage(const render::CommandBuffer& older,
                                    const render::CommandBuffer& newer) const {
    if (!layout::DrawTraversal::paintCacheEnabled()) {
        render::Damage all;
        all.setFull();
        return all;
    }
    return render::diffDamage(older, newer);
}

void Engine::replayAppLayers(render::SkiaRenderer* renderer,
                             const render::CommandBuffer& buffer,
                             std::vector<render::SkiaRenderer::LayerSurface>& pool,
                             int& poolW, int& poolH,
                             int surfW, int surfH,
                             std::vector<UILayer>& outLayers,
                             const render::CommandBuffer* promotedBuffer,
                             HtmlPoolState* held, uint64_t baseGen, float scale,
                             const render::DamageHistory* history) {
    if (!renderer) return;

    // surfW/surfH are the *content* dimensions (viewport minus engine-reserved
    // insets) — app layer surfaces are content-sized. The pool compare below
    // must use these dims so a menu show/hide (contentH change) reallocates.
    if (poolW != surfW || poolH != surfH) {
        pool.clear();
        poolW = surfW;
        poolH = surfH;
        if (held) held->baseGen = 0;
    }
    if (pool.empty()) {
        pool.push_back(renderer->createLayerSurface(surfW, surfH));
        if (held) held->baseGen = 0;  // fresh surfaces hold nothing yet
    }

    int htmlLayerIdx = 0;
    sk_sp<SkSurface> origSurface = sk_ref_sp(renderer->surface());

    // What of the base this pool copy has to draw again. Its surfaces are
    // drawn only here, so when it holds an earlier recording at this scale,
    // only what changed since (render/damage.h) is cleared and replayed, and
    // when it holds this one, nothing: its HTML layers stand and only the
    // promoted layer above them is drawn again.
    render::Damage damage;
    damage.setFull();
    if (held && held->baseGen != 0 && held->scale == scale &&
        held->htmlLayerIdx < static_cast<int>(pool.size())) {
        if (held->baseGen == baseGen) damage = render::Damage{};
        else if (!history || !history->since(held->baseGen, baseGen, damage)) damage.setFull();
    }
    if (damage.full && held) pageWholeHistory_.fetch_add(1, std::memory_order_relaxed);
    SkIRect clip = SkIRect::MakeEmpty();
    if (!damage.full && !damage.empty) {
        clip = SkRect::MakeLTRB(damage.x0 * scale, damage.y0 * scale, damage.x1 * scale, damage.y1 * scale)
                   .roundOut();
        // Rounded out to a 16 px grid, so a damage that moves by a pixel or
        // two between passes keeps the same rect (and the same scratch size).
        constexpr int kGrid = 16;
        clip.setLTRB(clip.fLeft & ~(kGrid - 1), clip.fTop & ~(kGrid - 1),
                     (clip.fRight + kGrid - 1) & ~(kGrid - 1), (clip.fBottom + kGrid - 1) & ~(kGrid - 1));
        if (!clip.intersect(SkIRect::MakeWH(surfW, surfH))) damage = render::Damage{};
    }
    // What a partial pass draws: the damage and everything that reaches it,
    // whole, with the clips it is under that are not rects (render::
    // coveringBounds), on the same 16 px grid. Drawn into a scratch that
    // size, nothing that touches the damage is cut by the scratch's edge
    // (the CPU antialiases a cut path otherwise), nor a rounded clip over
    // it (Ganesh draws under one by what cuts it), and the scratch sits a
    // multiple of 16 device px from the pool's corner (a gradient's dither
    // and the GPU's 2x2-quad derivatives line up). What cannot be bounded,
    // or most of the surface, is a whole pass.
    SkIRect area = clip;
    if (!damage.full && !damage.empty) {
        const SkRect cssClip = SkRect::MakeLTRB(clip.fLeft / scale, clip.fTop / scale, clip.fRight / scale,
                                                clip.fBottom / scale);
        SkRect cover;
        if (!render::coveringBounds(buffer, cssClip, cover)) {
            damage.setFull();
            pageWholeCover_.fetch_add(1, std::memory_order_relaxed);
        } else {
            area = SkRect::MakeLTRB(cover.fLeft * scale, cover.fTop * scale, cover.fRight * scale,
                                    cover.fBottom * scale)
                       .roundOut()
                       .makeOutset(2, 2);
            area.join(clip);
            constexpr int kGrid = 16;
            area.setLTRB(area.fLeft & ~(kGrid - 1), area.fTop & ~(kGrid - 1), (area.fRight + kGrid - 1) & ~(kGrid - 1),
                         (area.fBottom + kGrid - 1) & ~(kGrid - 1));
            if (!area.intersect(SkIRect::MakeWH(surfW, surfH)) ||
                static_cast<int64_t>(area.width()) * area.height() * 10 > static_cast<int64_t>(surfW) * surfH * 6) {
                damage.setFull();
                pageWholeArea_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    if (!damage.full && damage.empty) {
        outLayers = held->layers;
        htmlLayerIdx = held->htmlLayerIdx;
        held->baseGen = baseGen;
    } else {
    // A partial pass draws each HTML surface's area (above) into a scratch
    // surface of its own, then puts the damage rect back into the pool's
    // surface pixel for pixel. Drawn under a clip in place, the pixels along
    // the clip's edge came out antialiased differently from a whole pass
    // and stayed so, kept by every pass after.
    const bool partialPass = !damage.full;
    if (held) held->baseGen = 0;
    bool structureChanged = false;  // a surface new to the pool: the pass has to be whole
    auto enterHtml = [&](int k) {
        if (!partialPass) {
            renderer->switchSurface(pool[k].surface);
            return;
        }
        // The pool surface's own size, drawn under a clip to the area: its
        // device places, and the device it rasters against (the GPU maps
        // positions through the target's size), are exactly the pool's.
        auto& temps = held->scratch;
        if (static_cast<int>(temps.size()) <= k) temps.resize(k + 1);
        auto& t = temps[k];
        if (!t.surface || t.surface->width() != surfW || t.surface->height() != surfH)
            t = renderer->createLayerSurface(surfW, surfH);
        renderer->switchSurface(t.surface, &area);
    };
    auto leaveHtml = [&](int k) {
        if (!partialPass) return;
        renderer->putSurfaceRect(held->scratch[k].surface, {0, 0}, pool[k].surface, clip);
    };
    enterHtml(0);

    render::CommandReplayer replayer(renderer);
    // An HTML layer above a break that nothing drew into is left out: it
    // would composite as a transparent surface, and a frame that is one
    // client window with nothing over it can then be shown as that window's
    // buffer alone (direct scanout).
    uint64_t drawsAtSwitch = 0;
    // A scroll layer's thumbs are recorded after everything the base draws:
    // they go above its last HTML layer, splitting none.
    std::vector<UILayer> thumbs;
    replayer.setLayerBreakHandler([&](const render::LayerSource& source, const render::LayerQuad& quad) {
        if (const auto* sl = std::get_if<render::ScrollLayerSource>(&source);
            sl && (sl->part == render::ScrollLayerSource::ThumbV || sl->part == render::ScrollLayerSource::ThumbH)) {
            thumbs.push_back(UILayer::of(source, quad));
            return;
        }
        int prevIdx = htmlLayerIdx;
        htmlLayerIdx++;
        // A partial pass splits at the same breaks as the recording its pool
        // holds (damage is whole wherever the breaks differ): a surface new
        // to the pool means this one did not, and is drawn again whole.
        while (htmlLayerIdx >= static_cast<int>(pool.size())) {
            pool.push_back(renderer->createLayerSurface(surfW, surfH));
            if (partialPass) structureChanged = true;
        }
        leaveHtml(prevIdx);
        enterHtml(htmlLayerIdx);

        if (prevIdx == 0 || replayer.draws() != drawsAtSwitch) outLayers.push_back(htmlLayerOf(pool[prevIdx]));
        drawsAtSwitch = replayer.draws();
        outLayers.push_back(UILayer::of(source, quad));
    });

    replayer.replay(buffer);
    leaveHtml(htmlLayerIdx);

    // Capture the trailing HTML layer.
    renderer->switchSurface(origSurface);
    if (structureChanged) {
        // Not what a partial pass may do: the whole pass, from scratch.
        outLayers.clear();
        held->baseGen = 0;
        held->scale = 0.0f;
        replayAppLayers(renderer, buffer, pool, poolW, poolH, surfW, surfH, outLayers, promotedBuffer, held,
                        baseGen, scale, nullptr);
        return;
    }
    if (partialPass) pagePartialRasters_.fetch_add(1, std::memory_order_relaxed);
    if (partialPass && ScrollLayerSet::verifyPartial()) {
        // A check for tests (setScrollLayerVerify): the same recording whole,
        // split at the same breaks, beside the pool; every pixel the same.
        std::vector<render::SkiaRenderer::LayerSurface> whole;
        whole.push_back(renderer->createLayerSurface(surfW, surfH));
        renderer->switchSurface(whole[0].surface);
        render::CommandReplayer wr(renderer);
        wr.setLayerBreakHandler([&](const render::LayerSource& source, const render::LayerQuad&) {
            if (const auto* sl = std::get_if<render::ScrollLayerSource>(&source);
                sl && (sl->part == render::ScrollLayerSource::ThumbV || sl->part == render::ScrollLayerSource::ThumbH))
                return;
            whole.push_back(renderer->createLayerSurface(surfW, surfH));
            renderer->switchSurface(whole.back().surface);
        });
        wr.replay(buffer);
        renderer->switchSurface(origSurface);
        const SkImageInfo info = SkImageInfo::MakeN32Premul(surfW, surfH);
        std::vector<uint32_t> a(static_cast<size_t>(surfW) * surfH), b(a.size());
        render::SkiaGpu::Lock lock = renderer->skiaGpu() ? renderer->skiaGpu()->lock() : render::SkiaGpu::Lock();
        for (size_t k = 0; k < whole.size() && k <= static_cast<size_t>(htmlLayerIdx); ++k) {
            if (!whole[k].surface || !pool[k].surface) continue;
            if (!pool[k].surface->readPixels(info, a.data(), surfW * 4, 0, 0) ||
                !whole[k].surface->readPixels(info, b.data(), surfW * 4, 0, 0))
                continue;
            uint64_t n = 0;
            for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
            pagePartialMismatch_.fetch_add(n, std::memory_order_relaxed);
        }
    }
    if (htmlLayerIdx == 0 || replayer.draws() != drawsAtSwitch) outLayers.push_back(htmlLayerOf(pool[htmlLayerIdx]));
    outLayers.insert(outLayers.end(), thumbs.begin(), thumbs.end());
    if (held) {
        held->baseGen = baseGen;
        held->scale = scale;
        held->htmlLayerIdx = htmlLayerIdx;
        held->layers = outLayers;
    }
    }

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
        promotedReplayer.setLayerBreakHandler([](const render::LayerSource&, const render::LayerQuad&) {});
        promotedReplayer.replay(*promotedBuffer);
        renderer->switchSurface(origSurface);

        if (promotedReplayer.draws() != 0) outLayers.push_back(htmlLayerOf(pool[promotedIdx]));
    }
}

void Engine::recordSystemPanelLayers(render::CommandBuffer& outBuffer,
                                     int vpW, int vpH) {
    outBuffer.clear();
    systemDirty_ = false;
    if (!recordingRenderer_ || !drawTraversal_ || !isSystemVisible()) return;

    // Layout still runs on main thread using textMetrics_ (paired with
    // renderer_, which the recording renderer also delegates to).
    layoutSystemPanels(*textMetrics_);

    recordingRenderer_->setBuffer(&outBuffer);

    bool first = true;
    for (auto& sdoc : systemDocs_) {
        if (!isSystemDocVisible(sdoc) || !sdoc.document) continue;

        // Between panels, a surface break: the replayer captures the current
        // panel into a UILayer and starts a new surface.
        if (!first) recordingRenderer_->recordSurfaceBreak();
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
    replayer.setSurfaceBreakHandler([&]() {
        // Capture current panel into a UILayer, advance to next surface.
        outLayers.push_back(htmlLayerOf(pool[panelIdx]));

        panelIdx++;
        ensurePoolAt(panelIdx);
        renderer->fitLayerSurface(pool[panelIdx], vpW, vpH);
        renderer->switchSurface(pool[panelIdx].surface);
    });
    replayer.setBlitCanvasInlineHandler(
        [&](void* scenePtr, float x, float y, float w, float h) {
            auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
            if (!scene || w <= 0 || h <= 0) return;
            // The canvas's own surface is drawn and snapshotted directly.
            render::SkiaGpu::Lock lock =
                renderer->skiaGpu() ? renderer->skiaGpu()->lock() : render::SkiaGpu::Lock();
            scene->flushStaged();
            auto img = scene->displayImage();
            if (!img) return;
            auto* c = renderer->getCanvas();
            if (!c) return;
            SkRect dst = SkRect::MakeXYWH(x, y, w, h);
            c->drawImageRect(img, dst, SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });

    replayer.replay(buffer);

    // Capture the final panel.
    outLayers.push_back(htmlLayerOf(pool[panelIdx]));

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
        if (d->document && d->document->isDirty()) noteHoverRecheck();  // laid out again
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
        if (h->document->isDirty()) noteHoverRecheck();  // laid out again
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
