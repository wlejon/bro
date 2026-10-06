// Frame composition: a frame's layers as one PresentFrame — GPU images (3D
// scenes, WebGL canvases, and with Skia on the GPU every HTML, canvas and
// iframe layer) the presenter draws in place, and the CPU composite of any
// CPU-drawn layers between them — and its presentation: windowed through the
// VulkanPresenter (or the software window surface without Vulkan), headless
// as pixels for a capture.

#include "engine/engine.h"
#include "engine/frame_presenter.h"
#include "engine/terminal_layers.h"
#include "engine/window_host.h"

#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "platform/sdl_window.h"
#include "render/pixel_convert.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#include "util/log.h"
#include "webgl/webgl2_context.h"

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#endif

#if BRO_WITH_DMABUF
#include "render/vulkan_dmabuf_importer.h"
#include "render/kms_direct_presenter.h"
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <variant>

namespace bro::engine {

namespace {

// Where a layer lands in the composite (device px), and its clip.
struct LayerPlacement {
    float sx = 1.0f, sy = 1.0f, oy = 0.0f;

    SkRect dst(const render::LayerQuad& q) const {
        return SkRect::MakeXYWH(q.x * sx, (q.y + oy) * sy, q.w * sx, q.h * sy);
    }
    // The layer's clip, when it has one.
    bool clip(const render::LayerQuad& q, SkRect& out) const {
        if (!q.clipped()) return false;
        out = SkRect::MakeXYWH(q.clipX * sx, (q.clipY + oy) * sy, q.clipW * sx, q.clipH * sy);
        return true;
    }
    void draw(SkCanvas* canvas, const sk_sp<SkImage>& img, const render::LayerQuad& q) const {
        if (!img) return;
        canvas->save();
        SkRect c;
        if (clip(q, c)) canvas->clipRect(c, SkClipOp::kIntersect, true);
        canvas->drawImageRect(img, dst(q), SkSamplingOptions(SkFilterMode::kLinear));
        canvas->restore();
    }
};

template <class... Fs>
struct Overloaded : Fs... {
    using Fs::operator()...;
};
template <class... Fs>
Overloaded(Fs...) -> Overloaded<Fs...>;

} // namespace

// Start the next GPU frame: waits for the frame slot about to be reused (not
// the device) and runs the destructors deferred until it finished.
void Engine::beginGpuFrame() {
    if (vulkanContext_) vulkanContext_->frames().beginFrame();
}

// Size this frame's composite; forget last frame's layers. The CPU segments
// are made and cleared only once a CPU layer is drawn into them: a frame of
// GPU layers alone (Skia on the GPU) has none.
void Engine::beginFrameComposite() {
    if (skiaGpu_) skiaGpu_->collect();
    int fbW = deviceScale_.drawableW > 0 ? deviceScale_.drawableW : viewportWidth_;
    int fbH = deviceScale_.drawableH > 0 ? deviceScale_.drawableH : viewportHeight_;
    if (frameCompositeW_ != fbW || frameCompositeH_ != fbH) {
        frameSegments_.clear();
        frameCompositeW_ = fbW;
        frameCompositeH_ = fbH;
    }
    frameSegmentUsed_.clear();
    frameImages_.clear();
    frameSkiaImages_.clear();
}

// The segment the next CPU layer composites into: the one above the last GPU
// image, cleared the first time it is drawn into this frame. The bottom one is
// the frame's base and starts opaque black, so every frame (and every capture)
// is opaque; the ones above GPU images start transparent.
SkCanvas* Engine::frameSegmentCanvas() {
    const size_t index = frameImages_.size();
    while (frameSegments_.size() <= index)
        frameSegments_.push_back(SkSurfaces::Raster(SkImageInfo::MakeN32Premul(frameCompositeW_, frameCompositeH_)));
    if (frameSegmentUsed_.size() <= index) frameSegmentUsed_.resize(index + 1, false);
    SkSurface* surface = frameSegments_[index].get();
    if (!surface) return nullptr;
    if (!frameSegmentUsed_[index]) {
        surface->getCanvas()->clear(index == 0 ? SK_ColorBLACK : SK_ColorTRANSPARENT);
        frameSegmentUsed_[index] = true;
    }
    return surface->getCanvas();
}

void Engine::compositeLayers(const std::vector<UILayer>& layers, int offsetY) {
    if (layers.empty() || frameCompositeW_ <= 0 || frameCompositeH_ <= 0) return;

    const int fbW = frameCompositeW_, fbH = frameCompositeH_;
    LayerPlacement at;
    at.sx = static_cast<float>(fbW) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    at.sy = static_cast<float>(fbH) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    at.oy = static_cast<float>(offsetY);

    // A GPU layer's image goes to the presenter, placed and clipped where the
    // layer sits; the CPU layers after it composite into the next segment.
    auto place = [&](VkImage image, VkImageLayout layout, uint32_t w, uint32_t h, const SkRect& dst,
                     const render::LayerQuad* quad) -> render::PresentImage& {
        render::PresentImage& out = frameImages_.emplace_back();
        out.image = image;
        out.layout = layout;
        out.width = w;
        out.height = h;
        out.dstX = dst.left();
        out.dstY = dst.top();
        out.dstW = dst.width();
        out.dstH = dst.height();
        SkRect clip;
        if (quad && at.clip(*quad, clip)) {
            const SkIRect r = clip.roundOut();
            out.clipped = true;
            out.clip = {{r.left(), r.top()},
                        {static_cast<uint32_t>(std::max(0, r.width())), static_cast<uint32_t>(std::max(0, r.height()))}};
        }
        return out;
    };
    // A 3D scene's or WebGL canvas's image, sampled where it is. One never
    // drawn into (UNDEFINED) has nothing to show.
    auto placeLayerImage = [&](const render::LayerImage& img, const render::LayerQuad& q) {
        if (!img || img.layout == VK_IMAGE_LAYOUT_UNDEFINED) return;
        place(img.image, img.layout, img.width, img.height, at.dst(q), &q).view = img.view;
    };
    // A Skia GPU image, sampled where it is; kept alive until the frame has
    // been submitted.
    auto placeSkiaImage = [&](const render::SkiaImageRef& image, const SkRect& dst, const render::LayerQuad* quad) {
        place(image->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image->width, image->height, dst, quad).view =
            image->view;
        frameSkiaImages_.push_back(image);
    };

    for (const auto& layer : layers) {
        const render::LayerQuad& quad = layer.quad;
        std::visit(Overloaded{
            [&](const HtmlLayer& html) {
                // Content space, 1:1 in device px, below the inset.
                if (html.image) {
                    placeSkiaImage(html.image,
                                   SkRect::MakeXYWH(0.0f, at.oy * at.sy, static_cast<float>(html.image->width),
                                                    static_cast<float>(html.image->height)),
                                   nullptr);
                } else if (html.surface) {
                    if (auto img = html.surface->makeImageSnapshot())
                        if (SkCanvas* canvas = frameSegmentCanvas()) canvas->drawImage(img, 0.0f, at.oy * at.sy);
                }
            },
            [&](const render::IframeLayerSource& src) {
                IframeDoc* d = iframeDocById(src.docId);
                if (!d) return;
                if (render::SkiaImageRef image = d->published.gpu()) placeSkiaImage(image, at.dst(quad), &quad);
                else if (SkCanvas* canvas = frameSegmentCanvas()) at.draw(canvas, d->published.get(), quad);
            },
            [&](const render::TerminalLayerSource& src) {
                const PublishedFrame* p = terminalLayers_ ? terminalLayers_->published(src.layerId) : nullptr;
                if (!p) return;
                // Pixel for pixel at a whole device-pixel origin, so the
                // glyphs stay as crisp as they were rasterized.
                auto dstFor = [&](int w, int h) {
                    const SkRect d = at.dst(quad);
                    return SkRect::MakeXYWH(std::round(d.left()), std::round(d.top()), float(w), float(h));
                };
                if (render::SkiaImageRef image = p->gpu()) {
                    placeSkiaImage(image, dstFor(int(image->width), int(image->height)), &quad);
                } else if (sk_sp<SkImage> img = p->get()) {
                    if (SkCanvas* canvas = frameSegmentCanvas()) {
                        canvas->save();
                        SkRect c;
                        if (at.clip(quad, c)) canvas->clipRect(c, SkClipOp::kIntersect, true);
                        const SkRect d = dstFor(img->width(), img->height());
                        canvas->drawImage(img, d.left(), d.top());
                        canvas->restore();
                    }
                }
            },
            [&](const render::CanvasLayerSource& src) {
                canvas::CanvasScene* cs = canvasSceneById(src.sceneId);
                if (!cs) return;
                if (render::SkiaImageRef image = cs->gpuImage()) placeSkiaImage(image, at.dst(quad), &quad);
                else if (cs->surface())
                    if (SkCanvas* canvas = frameSegmentCanvas())
                        at.draw(canvas, cs->surface()->makeImageSnapshot(), quad);
            },
            [&](const render::SceneLayerSource& src) {
#if BRO_WITH_3D
                for (auto& sg : sceneGraphs_) {
                    if (!sg.graph || sg.elementId != src.elementId) continue;
                    if (sg.graph->renderer().hasMeshContent())
                        placeLayerImage(sg.graph->renderer().outputImage(), quad);
                    return;
                }
#else
                (void)src;
#endif
            },
            [&](const render::WebGLLayerSource& src) {
                for (auto& entry : webglEntries_) {
                    if (!entry.context || !entry.element || entry.element->nodeId() != src.elementId) continue;
                    // The canvas's recorded work must be submitted before the
                    // presenter's submission samples it (queue order does the rest).
                    entry.context->flush();
                    placeLayerImage(entry.context->drawingBuffer(), quad);
                    return;
                }
            },
            [&](const render::DmabufLayerSource& src) {
#if BRO_WITH_DMABUF
                if (!vulkanPresenter_ || !vulkanPresenter_->dmabufImporter()) return;
                static uint64_t s_dmabufFrameCounter = 0;
                ++s_dmabufFrameCounter;
                auto* buf = vulkanPresenter_->dmabufImporter()->getOrImport(src, s_dmabufFrameCounter);
                if (!buf || buf->image == VK_NULL_HANDLE) return;
                if (vulkanPresenter_->kmsDirectPresenter() &&
                    vulkanPresenter_->kmsDirectPresenter()->canDirectScanout(src, quad, vulkanPresenter_->width(), vulkanPresenter_->height())) {
                    vulkanPresenter_->kmsDirectPresenter()->directScanout(src);
                    return;
                }
                place(buf->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, buf->width, buf->height, at.dst(quad), &quad).view = buf->view;
#else
                (void)src;
#endif
            },
        }, layer.content);
    }
}

static render::PresentPixels layerOf(SkSurface* surface) {
    return surface ? render::VulkanPresenter::surfaceLayer(surface) : render::PresentPixels{};
}

// The frame as a PresentFrame: the first segment below, then each GPU image
// with the segment composited after it above, over transparency, at the
// composite's size.
render::PresentFrame Engine::describeCompositedFrame() {
    render::PresentFrame frame;
    auto segmentUsed = [&](size_t i) {
        return i < frameSegmentUsed_.size() && frameSegmentUsed_[i] && i < frameSegments_.size();
    };
    if (segmentUsed(0)) frame.below = layerOf(frameSegments_[0].get());
    frame.images = std::move(frameImages_);
    for (size_t i = 0; i < frame.images.size(); ++i)
        if (segmentUsed(i + 1)) frame.images[i].above = layerOf(frameSegments_[i + 1].get());
    frame.clearColor[0] = frame.clearColor[1] = frame.clearColor[2] = 0.0f;
    frame.clearColor[3] = 1.0f;   // opaque black under everything
    frame.width = static_cast<uint32_t>(std::max(0, frameCompositeW_));
    frame.height = static_cast<uint32_t>(std::max(0, frameCompositeH_));
    frameImages_.clear();
    frameSegmentUsed_.clear();
    return frame;
}

void Engine::presentCurrentFrame() {
    const render::PresentFrame frame = describeCompositedFrame();
    if (vulkanPresenter_ && !vulkanPresenter_->isHeadless()) {
        if (!vulkanPresenter_->present(frame)) LOG_ERROR("Engine: presenting the frame failed");
        frameSkiaImages_.clear();  // submitted
    } else if (window_ && window_->backend() == platform::GraphicsBackend::Software && frame.below) {
        // No GPU, so no GPU layer: the CPU composite is the frame.
        const render::PresentPixels& p = frame.below;
        window_->presentPixels(p.pixels, static_cast<int>(p.width), static_cast<int>(p.height),
                               static_cast<int>(p.stride), p.bgra);
    }
}

// Headless capture of the composited frame as RGBA8. A frame that is only the
// CPU composite is read straight from it; one with GPU layers is composited by
// the presenter and read back, in one submission and one wait.
std::vector<uint8_t> Engine::readCompositedFrame() {
    const bool cpuOnly = frameImages_.empty();
    if (cpuOnly) frameSegmentCanvas();  // a frame nothing was drawn into is black
    const render::PresentFrame frame = describeCompositedFrame();
    if (!cpuOnly && vulkanPresenter_ && vulkanPresenter_->isHeadless()) {
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        const bool ok = vulkanPresenter_->present(frame) && vulkanPresenter_->readbackPixels(pixels, w, h);
        frameSkiaImages_.clear();
        if (ok) return pixels;
        LOG_ERROR("Engine: GPU frame composite/readback failed");
        return {};
    }
    SkPixmap pm;
    if (frameSegments_.empty() || !frameSegments_[0] || !frameSegments_[0]->peekPixels(&pm)) return {};
    return render::pixmapToRgba(pm);
}

bool Engine::capturePresentsRequested() {
    const char* v = std::getenv("BRO_CAPTURE_PRESENTS");
    return v && std::strcmp(v, "1") == 0;
}

std::vector<uint8_t> Engine::presentedPixels(uint64_t hostId, int& outW, int& outH) {
    outW = outH = 0;
    render::VulkanPresenter* presenter = nullptr;
    if (hostId == 0) {
        presenter = vulkanPresenter_.get();
    } else if (WindowHost* h = windowHostById(hostId)) {
        presenter = h->presenter.get();
    }
    if (!presenter || presenter->isHeadless()) return {};
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    if (!presenter->readbackPixels(pixels, w, h)) return {};
    outW = static_cast<int>(w);
    outH = static_cast<int>(h);
    return pixels;
}

} // namespace bro::engine
