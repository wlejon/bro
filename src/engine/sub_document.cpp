// Shared sub-document core — see sub_document.h for why this is a set of free
// functions over a SubDocRef view rather than a common base struct.

#include "engine/sub_document.h"

#include "bronze_host/bronze_host.h"

#include "engine/default_styles.h"
#include "engine/app_loader.h"
#include "engine/replaced_elements.h"
#include "engine/settings.h"
#include "util/log.h"
#include "layout/box.h"
#include "layout/element_ref_adapter.h"
#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include "render/renderer.h"
#include "render/command_buffer.h"
#include "engine/device_scale.h"
#include "render/command_replayer.h"
#include "render/recording_renderer.h"
#include "render/skia_backend.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/element_geometry.h"
#include "canvas/canvas_scene.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace bro::engine {

SubDocRef Engine::iframeSubDoc(IframeDoc& d) {
    return SubDocRef{d.canvasScenes, d.document,
                     d.hoveredElement, d.boxW, d.boxH, d.cmdBuffer,
                     d.surface, d.spare, d.surfW, d.surfH, d.published};
}

SubDocRef Engine::windowHostSubDoc(WindowHost& h) {
    return SubDocRef{h.canvasScenes, h.document,
                     h.hoveredElement, h.boxW, h.boxH, h.cmdBuffer,
                     h.surface, h.spare, h.surfW, h.surfH, h.published};
}

SubDocSource loadSubDocSource(const std::string& basePath, const std::string& srcAttr,
                              const util::AssetMounts* mounts, const char* what) {
    SubDocSource out;
    if (srcAttr.empty()) return out;

    out.resolvedSrc = AppLoader::resolvePath(basePath, srcAttr, mounts);

    std::error_code ec;
    out.appDir = out.resolvedSrc;
    if (!fs::is_directory(out.resolvedSrc, ec)) {
        if (!fs::is_regular_file(out.resolvedSrc, ec)) {
            LOG_WARN("%s: failed to resolve '%s'", what, srcAttr.c_str());
            return out;
        }
        out.appDir = fs::path(out.resolvedSrc).parent_path().string();
    }

    out.manifest = AppLoader::loadApp(out.appDir, mounts);
    out.html = AppLoader::loadFile(out.manifest.htmlPath);
    if (out.html.empty()) {
        LOG_WARN("%s: no index.html at '%s' (src='%s')", what,
                 out.appDir.c_str(), srcAttr.c_str());
        return out;
    }
    for (auto& cssPath : out.manifest.stylePaths) {
        std::string css = AppLoader::loadFile(cssPath);
        if (!css.empty()) {
            out.authorStyles += css + "\n";
        }
    }
    out.ok = true;
    return out;
}

void buildSubDocDocument(SubDocRef d, const SubDocSource& src,
                         const std::string& colorScheme, float resolution) {
    d.document = std::make_unique<dom::Document>();
    d.document->setBasePath(src.manifest.basePath);
    d.document->setMediaColorScheme(colorScheme);
    d.document->setMediaResolution(resolution);
    d.document->setMediaViewport(static_cast<float>(d.boxW), static_cast<float>(d.boxH));
    d.document->parse(src.html, src.authorStyles, kDefaultStyles);
}

void runSubDocScripts(SubDocRef d, const SubDocSource& src, Engine* engine, bool isChild) {
    if (!engine || !d.document) return;
    bro::bronze_host::runHostSubDocScripts(*engine, d.document.get(),
                                           src.manifest.scripts,
                                           src.appDir, src.manifest.basePath,
                                           isChild);
}

void finishSubDocLoad(SubDocRef d, const SubDocSource& src,
                      render::Renderer* renderer, broaudio::Engine* audio,
                      layout::SkiaTextMetrics& metrics) {
    bro::engine::ensureReplacedElements(d.document->documentElement(), renderer, audio);
    d.document->resolveStyles();
    d.document->performLayout(static_cast<float>(d.boxW),
                              static_cast<float>(d.boxH), metrics);
    d.document->setBasePath(src.manifest.basePath);
}

static void collectNestedIframes(dom::Element* el, std::vector<dom::Element*>& out) {
    if (!el) return;
    std::string_view tag = el->tagName();
    if (tag == "iframe" || tag == "IFRAME") out.push_back(el);
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element)
            collectNestedIframes(static_cast<dom::Element*>(child), out);
    }
}

void warnNestedIframes(SubDocRef d, const char* what) {
    if (!d.document) return;
    std::vector<dom::Element*> frames;
    collectNestedIframes(d.document->documentElement(), frames);
    for (auto* f : frames) {
        std::string src = f->getAttribute("src");
        LOG_WARN("%s: nested <iframe src=\"%s\"> is not supported — "
                 "the element renders as an empty box", what, src.c_str());
    }
}

bool tickSubDoc(SubDocRef d, double /*nowMs*/) {
    if (d.cmdBuffer.commandCount() == 0) return true;
    if (d.document && d.document->isDirty()) return true;
    return false;
}

void recordSubDoc(SubDocRef d, render::RecordingRenderer* rec,
                  layout::DrawTraversal* traversal, layout::SkiaTextMetrics& metrics) {
    d.cmdBuffer.clear();
    if (!d.document || !d.document->documentElement()) return;
    layout::ElementRefAdapter::setHoveredElement(d.hoveredElement);
    d.document->resolveStyles();
    d.document->performLayout(static_cast<float>(d.boxW),
                              static_cast<float>(d.boxH), metrics);
    for (auto& scene : d.canvasScenes) {
        if (scene) scene->stageCommandsForRaster();
    }
    rec->setBuffer(&d.cmdBuffer);
    // A sub-document is one surface: its canvases draw inline. (WebGL, 3D
    // scenes and nested iframes are not hosted in sub-documents.)
    traversal->setLayerBreakCallback([rec](const layout::DrawTraversal::LayerBreak& lb) {
        if (!std::holds_alternative<render::CanvasLayerSource>(lb.source)) return;
        if (void* scene = lb.element->canvasScene())
            rec->recordBlitCanvasInline(scene, lb.quad.x, lb.quad.y, lb.quad.w, lb.quad.h);
    });
    traversal->setBasePath(d.document->basePath());
    traversal->draw(d.document->documentElement(), 0, 0,
                    static_cast<float>(d.boxW), static_cast<float>(d.boxH),
                    /*viewportTop=*/0);
    traversal->setLayerBreakCallback(nullptr);
    rec->setBuffer(nullptr);
}

static void replayBufferWithInlineCanvas(render::SkiaRenderer* renderer,
                                         const render::CommandBuffer& buffer) {
    render::CommandReplayer replayer(renderer);
    replayer.setBlitCanvasInlineHandler(
        [&](void* scenePtr, float x, float y, float w, float h) {
            auto* scene = static_cast<canvas::CanvasScene*>(scenePtr);
            if (!scene || w <= 0 || h <= 0) return;
            // The canvas's own surface is drawn and snapshotted directly.
            render::SkiaGpu::Lock lock =
                renderer->skiaGpu() ? renderer->skiaGpu()->lock() : render::SkiaGpu::Lock();
            scene->flushStaged();
            auto* src = scene->surface();
            if (!src) return;
            auto img = src->makeImageSnapshot();
            if (!img) return;
            auto* c = renderer->getCanvas();
            if (!c) return;
            c->drawImageRect(img, SkRect::MakeXYWH(x, y, w, h),
                             SkSamplingOptions(SkFilterMode::kLinear));
            scene->clearDirty();
        });
    replayer.replay(buffer);
}

void replaySubDoc(SubDocRef d, render::SkiaRenderer* renderer) {
    if (d.cmdBuffer.commandCount() == 0) { d.published.clear(); return; }
    // The surface is in device px at the renderer's current scale; the box
    // (and the compositor quad sampling it) stay in CSS px.
    DeviceScale ds;
    ds.render = renderer->deviceScale();
    int bw = ds.toDevice(std::max(1, d.boxW)), bh = ds.toDevice(std::max(1, d.boxH));
    if (d.surfW != bw || d.surfH != bh) {
        d.spare.reset();
        d.surfW = bw; d.surfH = bh;
    }
    renderer->fitLayerSurface(d.surface, bw, bh);
    if (!d.surface) { d.published.clear(); return; }
    auto prev = renderer->switchSurface(d.surface.surface);
    replayBufferWithInlineCanvas(renderer, d.cmdBuffer);
    renderer->switchSurface(prev);
    // The compositor reads what is published, never the surface being drawn:
    // a CPU snapshot, or the GPU image once the frame's work is submitted.
    if (d.surface.isGpu()) {
        renderer->afterSubmit([&published = d.published, image = d.surface.image]() mutable {
            published.publish(std::move(image));
        });
        std::swap(d.surface, d.spare);
    } else {
        d.published.publish(d.surface.surface->makeImageSnapshot());
    }
}

std::vector<uint8_t> captureSubDoc(SubDocRef d, render::SkiaRenderer* skia,
                                   int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (d.cmdBuffer.commandCount() == 0) return {};
    int w = std::max(1, d.boxW), h = std::max(1, d.boxH);

    render::LayerSurface surf = skia->createLayerSurface(w, h);
    if (!surf.surface) return {};
    // capture() hands back CSS-px pixels whatever the display scale.
    const float prevScale = skia->deviceScale();
    skia->setDeviceScale(1.0f);
    auto prev = skia->switchSurface(surf.surface);
    if (auto* c = skia->getCanvas()) c->clear(SK_ColorTRANSPARENT);

    replayBufferWithInlineCanvas(skia, d.cmdBuffer);
    // Switching away applies (on the GPU, also finishes) what was drawn.
    skia->switchSurface(prev);
    skia->setDeviceScale(prevScale);

    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    {
        render::SkiaGpu::Lock lock = skia->skiaGpu() ? skia->skiaGpu()->lock() : render::SkiaGpu::Lock();
        if (!surf.surface->readPixels(info, pixels.data(), w * 4, 0, 0)) pixels.clear();
    }
    skia->releaseLayerSurface(surf);

    if (pixels.empty()) return {};
    outW = w;
    outH = h;
    return pixels;
}

void teardownSubDoc(SubDocRef d) {
    if (d.document) {
        dom::Document* doc = d.document.get();
        bro::bronze_host::clearHostTimersForDocument(doc);
        bro::bronze_host::clearHostAnimationFramesForDocument(doc);
        bro::bronze_host::clearHostElementsForDocument(doc);
        bro::bronze_host::clearRealmScope(bro::bronze_host::scopeIdForDocument(doc));
        bro::bronze_host::clearHostDocument(doc);
    }
    d.document.reset();
}

} // namespace bro::engine
