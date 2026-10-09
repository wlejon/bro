// Frame composition: a frame's layers as one PresentFrame — GPU images (3D
// scenes, WebGL canvases, and with Skia on the GPU every HTML, canvas and
// iframe layer) the presenter draws in place, and the CPU composite of any
// CPU-drawn layers between them — and its presentation: windowed through the
// VulkanPresenter (or the software window surface without Vulkan), headless
// as pixels for a capture.

#include "engine/engine.h"
#include "engine/app_runtime.h"
#include "engine/frame_presenter.h"
#include "engine/frame_trace.h"
#include "engine/terminal_layers.h"
#include "engine/window_host.h"

#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "platform/window.h"
#include "render/pixel_convert.h"
#include "render/vulkan_context.h"
#include "render/vulkan_presenter.h"
#include "util/log.h"
#include "webgl/webgl2_context.h"

#if BRO_WITH_3D
#include "scene/scene_graph.h"
#include "scene/scene_renderer.h"
#endif

#if BRO_WITH_COMPOSITOR
#include "compositor/wayland_compositor.h"
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

#include <bit>
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

#if BRO_WITH_COMPOSITOR
// Which client frames a frame samples: each surface's frame sequence, which
// a new buffer (or a commit that changed what it shows) advances.
uint64_t clientFramesKey(const std::vector<compositor::LeasedSurfaceFrame>& leased) {
    uint64_t k = leased.size();
    for (const auto& lf : leased) {
        k = k * 0x100000001b3ull ^ lf.surfaceId;
#if BRO_HAVE_WAYLAND_SERVER
        k = k * 0x100000001b3ull ^ lf.frame.sequence;
        k = k * 0x100000001b3ull ^ lf.frame.image_id;
#endif
    }
    return k;
}
#endif

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
    directImage_ = SIZE_MAX;
    frameKey_ = 0x84222325cbf29ce4ull;
    frameVolatile_ = false;
    frameKeyAdd(static_cast<uint64_t>(fbW) << 32 | static_cast<uint32_t>(fbH));
    if (framePresenter_) frameKeyAdd(framePresenter_->generation());
}

void Engine::frameKeyAdd(uint64_t v) {
    frameKey_ ^= v + 0x9e3779b97f4a7c15ull + (frameKey_ << 6) + (frameKey_ >> 2);
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
        frameKeyAdd((uint64_t)(image));  // a pointer or a 64-bit handle, by platform
        frameKeyAdd(static_cast<uint64_t>(w) << 32 | h);
        for (float f : {out.dstX, out.dstY, out.dstW, out.dstH}) frameKeyAdd(std::bit_cast<uint32_t>(f));
        if (out.clipped)
            frameKeyAdd(static_cast<uint64_t>(static_cast<uint32_t>(out.clip.offset.x)) << 32 ^
                        static_cast<uint32_t>(out.clip.offset.y) ^ static_cast<uint64_t>(out.clip.extent.width) << 16 ^
                        static_cast<uint64_t>(out.clip.extent.height) << 40);
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
                frameKeyAdd(d->published.generation());
                if (render::SkiaImageRef image = d->published.gpu()) placeSkiaImage(image, at.dst(quad), &quad);
                else if (SkCanvas* canvas = frameSegmentCanvas()) at.draw(canvas, d->published.get(), quad);
            },
            [&](const render::TerminalLayerSource& src) {
                const PublishedFrame* p = terminalLayers_ ? terminalLayers_->published(src.layerId) : nullptr;
                if (!p) return;
                frameKeyAdd(p->generation());
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
                frameKeyAdd(cs->contentGeneration());
                if (render::SkiaImageRef image = cs->gpuImage()) placeSkiaImage(image, at.dst(quad), &quad);
                else if (cs->surface())
                    if (SkCanvas* canvas = frameSegmentCanvas())
                        at.draw(canvas, cs->surface()->makeImageSnapshot(), quad);
            },
            [&](const render::SceneLayerSource& src) {
#if BRO_WITH_3D
                frameVolatile_ = true;  // rendered every frame
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
                frameVolatile_ = true;  // its drawing buffer is redrawn in place
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
                // Placed like any other image; whether the frame can be shown
                // as this buffer alone is decided once the frame is complete
                // (presentDirectScanout: nothing may be drawn over it).
                const SkRect dst = at.dst(quad);
                place(buf->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, buf->width, buf->height, dst, &quad).view = buf->view;
                directImage_ = frameImages_.size() - 1;
                directSrc_ = src;
                directQuad_ = render::LayerQuad{};
                directQuad_.x = dst.left();
                directQuad_.y = dst.top();
                directQuad_.w = dst.width();
                directQuad_.h = dst.height();
                if (frameImages_.back().clipped) {
                    directQuad_.clipX = static_cast<float>(frameImages_.back().clip.offset.x);
                    directQuad_.clipY = static_cast<float>(frameImages_.back().clip.offset.y);
                    directQuad_.clipW = static_cast<float>(frameImages_.back().clip.extent.width);
                    directQuad_.clipH = static_cast<float>(frameImages_.back().clip.extent.height);
                }
#else
                (void)src;
#endif
            },
            [&](const render::ClientWindowsLayerSource& src) {
#if BRO_WITH_COMPOSITOR
                if (!drmCtx_ || !drmCtx_->compositor) return;
                drmCtx_->clientLayersComposited = true;
                // The run this break names, from the pass that recorded it.
                std::span<const render::ClientWindowRef> run;
                if (const auto* refs = drmCtx_->frames.list(src.list)) {
                    const size_t first = std::min<size_t>(src.first, refs->size());
                    const size_t count = std::min<size_t>(src.count, refs->size() - first);
                    run = std::span<const render::ClientWindowRef>(refs->data() + first, count);
                }
                std::vector<engine::UILayer> clientLayers;
                auto leased = drmCtx_->compositor->acquireClientLayers(run, src.parts, clientLayers);
                frameKeyAdd(clientFramesKey(leased));
                if (!clientLayers.empty()) compositeLayers(clientLayers);
                drmCtx_->leasedFrames.insert(drmCtx_->leasedFrames.end(), std::make_move_iterator(leased.begin()),
                                             std::make_move_iterator(leased.end()));
#else
                (void)src;
#endif
            },
        }, layer.content);
    }
}

// The client windows of a frame whose paint pass had no client-window break
// (an app that is not a shell, a compile frame): all of them, on top.
void Engine::compositeRemainingClientWindows() {
#if BRO_WITH_COMPOSITOR
    if (!drmCtx_ || !drmCtx_->compositor || drmCtx_->clientLayersComposited) return;
    drmCtx_->clientLayersComposited = true;
    std::vector<engine::UILayer> clientLayers;
    auto leased = drmCtx_->compositor->acquireClientLayers(clientLayers);
    frameKeyAdd(clientFramesKey(leased));
    if (!clientLayers.empty()) compositeLayers(clientLayers);
    drmCtx_->leasedFrames.insert(drmCtx_->leasedFrames.end(), std::make_move_iterator(leased.begin()),
                                 std::make_move_iterator(leased.end()));
#endif
}

// Leased client frames go back once the frame that sampled them is done.
void Engine::releaseClientWindowFrames() {
#if BRO_WITH_COMPOSITOR
    if (!drmCtx_ || !drmCtx_->compositor || drmCtx_->leasedFrames.empty()) return;
    // A present returns once its flip is committed; the flip landing is when
    // these client frames reach the screen, so they wait for it
    // (drmFlipLanded). Without a flip in flight (a failed commit) they go
    // back now, with frame callbacks only.
#if BRO_WITH_DMABUF
    if (auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr) {
        if (kms->flipPending()) {
            auto& held = drmCtx_->flipLeases;
            held.insert(held.end(), std::make_move_iterator(drmCtx_->leasedFrames.begin()),
                        std::make_move_iterator(drmCtx_->leasedFrames.end()));
            drmCtx_->leasedFrames.clear();
            return;
        }
    }
#endif
    drmCtx_->compositor->releaseClientLayers(drmCtx_->leasedFrames, nullptr);
    drmCtx_->leasedFrames.clear();
#endif
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

// A frame that is one client buffer covering the screen, with nothing drawn
// over it (no shell bar or pill, no software cursor: anything drawn after the
// buffer lands in the segment above it), is flipped to as that buffer: no
// composite, no GPU work, and the client's frame reaches the screen without a
// copy. Its framebuffer stays alive while it is on screen (the presenter
// keeps it), and its lease until the next flip has replaced it
// (drmFlipLanded). What is under it does not show.
bool Engine::presentDirectScanout() {
#if BRO_WITH_DMABUF
    if (displayMode_ != DisplayMode::Drm || !drmCtx_ || directImage_ == SIZE_MAX) return false;
    auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
    if (!kms) return false;
    uint8_t& miss = frameTrace_->current().scanoutMiss;
    if (directImage_ + 1 != frameImages_.size()) return miss = 1, false;  // something GPU-drawn above it
    if (directImage_ + 1 < frameSegmentUsed_.size() && frameSegmentUsed_[directImage_ + 1]) return miss = 2, false;
    if (static_cast<uint32_t>(frameCompositeW_) != kms->width() ||
        static_cast<uint32_t>(frameCompositeH_) != kms->height())
        return miss = 3, false;
    if (!kms->canDirectScanout(directSrc_, directQuad_, kms->width(), kms->height())) return miss = 4, false;
    if (!kms->directScanout(directSrc_)) return miss = 5, false;
    drmCtx_->flipIsDirect = true;
    return true;
#else
    return false;
#endif
}

bool Engine::holdUnchangedFrame() {
    if (frameVolatile_ || !presentedKeyValid_ || frameKey_ != presentedKey_) return false;
    if (displayMode_ == DisplayMode::Drm) {
#if BRO_WITH_DMABUF
        auto* kms = vulkanPresenter_ ? vulkanPresenter_->kmsDirectPresenter() : nullptr;
        if (!kms) return false;
        // The pointer moved over a picture that did not change: the cursor
        // plane alone, at the next vblank.
        if (kms->cursorChanged()) {
            if (!kms->presentCursorOnly()) return false;
            drmFlipFrame_ = frameNumber_;
            frameTrace_->current().presented = 3;
        }
#else
        return false;
#endif
    } else if (displayMode_ != DisplayMode::Windowed || !window_ || !window_->holdFrame()) {
        return false;
    }
    frameImages_.clear();
    frameSegmentUsed_.clear();
    frameSkiaImages_.clear();
    heldFrame_ = frameNumber_;
    return true;
}

bool Engine::presentCurrentFrame(bool mayHold) {
    if (mayHold && holdUnchangedFrame()) return false;
    const uint64_t key = frameKey_;
    presentedKeyValid_ = false;
    if (presentDirectScanout()) {
        frameImages_.clear();
        frameSegmentUsed_.clear();
        frameSkiaImages_.clear();
        noteFramePresented();
        drmFlipFrame_ = frameNumber_;
        frameTrace_->current().presented = 2;  // scanned out directly; its flip fills in the vblank
        presentedKey_ = key;
        presentedKeyValid_ = mayHold;
        return true;
    }
    const render::PresentFrame frame = describeCompositedFrame();
    bool presented = false;
    if (vulkanPresenter_ && !vulkanPresenter_->isHeadless()) {
        if (!vulkanPresenter_->present(frame)) {
            LOG_ERROR("Engine: presenting the frame failed");
        } else {
            noteFramePresented();  // the first one logs the launch's time to it
            drmFlipFrame_ = frameNumber_;  // a KMS flip landing later belongs to this frame
            presented = true;
        }
        frameSkiaImages_.clear();  // submitted
    } else if (window_ && window_->backend() == platform::GraphicsBackend::Software && frame.below) {
        // No GPU, so no GPU layer: the CPU composite is the frame.
        const render::PresentPixels& p = frame.below;
        presented = window_->presentPixels(p.pixels, static_cast<int>(p.width), static_cast<int>(p.height),
                                           static_cast<int>(p.stride), p.bgra);
    }
    // Only a frame presented by the path that may hold its repeats keys
    // them: a compile or panel frame in between always re-presents.
    presentedKey_ = key;
    presentedKeyValid_ = presented && mayHold;
    return presented;
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
    if (v && std::strcmp(v, "1") == 0) return true;
    // The agent control socket's screenshot of a windowed bro reads back the
    // last present: asking for the socket (BRO_CONTROL, any value but 0/off)
    // keeps one.
    const char* c = std::getenv("BRO_CONTROL");
    return c && *c && std::strcmp(c, "0") != 0 && std::strcmp(c, "off") != 0;
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
