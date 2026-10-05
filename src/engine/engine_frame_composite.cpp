// Frame composition: the CPU composite of a frame's UI layers around the GPU
// layers (3D scenes, WebGL canvases) the presenter draws in place, and its
// presentation — windowed through the VulkanPresenter (or the software window
// surface without Vulkan), headless as pixels for a capture.

#include "engine/engine.h"
#include "engine/frame_presenter.h"

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

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkSurface.h>

#include <cmath>

namespace bro::engine {

namespace {

// Where a layer lands in the composite (device px), and its clip.
struct LayerPlacement {
    float sx = 1.0f, sy = 1.0f, oy = 0.0f;

    SkRect dst(const UILayer& l) const {
        return SkRect::MakeXYWH(l.cx * sx, (l.cy + oy) * sy, l.cw * sx, l.ch * sy);
    }
    // The layer's clip, when it has one.
    bool clip(const UILayer& l, SkRect& out) const {
        if (l.clipW < 0.0f || l.clipH < 0.0f) return false;
        out = SkRect::MakeXYWH(l.clipX * sx, (l.clipY + oy) * sy, l.clipW * sx, l.clipH * sy);
        return true;
    }
    void draw(SkCanvas* canvas, const sk_sp<SkImage>& img, const UILayer& l) const {
        if (!img) return;
        canvas->save();
        SkRect c;
        if (clip(l, c)) canvas->clipRect(c, SkClipOp::kIntersect, true);
        canvas->drawImageRect(img, dst(l), SkSamplingOptions(SkFilterMode::kLinear));
        canvas->restore();
    }
};

} // namespace

// Start the next GPU frame: waits for the frame slot about to be reused (not
// the device) and runs the destructors deferred until it finished.
void Engine::beginGpuFrame() {
    if (vulkanContext_) vulkanContext_->frames().beginFrame();
}

// Size and clear this frame's composite; forget last frame's GPU layers.
void Engine::beginFrameComposite() {
    int fbW = deviceScale_.drawableW > 0 ? deviceScale_.drawableW : viewportWidth_;
    int fbH = deviceScale_.drawableH > 0 ? deviceScale_.drawableH : viewportHeight_;
    if (frameCompositeW_ != fbW || frameCompositeH_ != fbH) {
        frameSegments_.clear();
        frameCompositeW_ = fbW;
        frameCompositeH_ = fbH;
    }
    if (frameSegments_.empty())
        frameSegments_.push_back(SkSurfaces::Raster(SkImageInfo::MakeN32Premul(fbW, fbH)));
    if (frameSegments_[0]) frameSegments_[0]->getCanvas()->clear(SK_ColorTRANSPARENT);
    frameSegmentUsed_.assign(1, true);
    frameImages_.clear();
}

// The segment the next CPU layer composites into: the one above the last GPU
// image, cleared the first time it is drawn into this frame.
SkCanvas* Engine::frameSegmentCanvas() {
    const size_t index = frameImages_.size();
    while (frameSegments_.size() <= index)
        frameSegments_.push_back(SkSurfaces::Raster(SkImageInfo::MakeN32Premul(frameCompositeW_, frameCompositeH_)));
    if (frameSegmentUsed_.size() <= index) frameSegmentUsed_.resize(index + 1, false);
    SkSurface* surface = frameSegments_[index].get();
    if (!surface) return nullptr;
    if (!frameSegmentUsed_[index]) {
        surface->getCanvas()->clear(SK_ColorTRANSPARENT);
        frameSegmentUsed_[index] = true;
    }
    return surface->getCanvas();
}

void Engine::compositeLayers(const std::vector<UILayer>& layers, int offsetY) {
    if (layers.empty() || frameSegments_.empty() || !frameSegments_[0]) return;

    const int fbW = frameCompositeW_, fbH = frameCompositeH_;
    LayerPlacement at;
    at.sx = static_cast<float>(fbW) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    at.sy = static_cast<float>(fbH) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    at.oy = static_cast<float>(offsetY);

    // A GPU layer's image goes to the presenter, placed and clipped where the
    // layer sits; the CPU layers after it composite into the next segment.
    auto addImage = [&](VkImage image, VkImageLayout layout, uint32_t w, uint32_t h, const UILayer& l) {
        const SkRect dst = at.dst(l);
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
        if (at.clip(l, clip)) {
            const SkIRect r = clip.roundOut();
            out.clipped = true;
            out.clip = {{r.left(), r.top()},
                        {static_cast<uint32_t>(std::max(0, r.width())), static_cast<uint32_t>(std::max(0, r.height()))}};
        }
    };

    for (const auto& layer : layers) {
        if (layer.type == UILayer::HTML) {
            if (layer.surface) {
                if (auto img = layer.surface->makeImageSnapshot())
                    if (SkCanvas* canvas = frameSegmentCanvas()) canvas->drawImage(img, 0.0f, at.oy * at.sy);
            }
        } else if (layer.type == UILayer::Iframe) {
            if (auto* d = iframeDocById(layer.canvasSceneId))
                if (SkCanvas* canvas = frameSegmentCanvas()) at.draw(canvas, d->published.get(), layer);
        } else if (layer.type == UILayer::Canvas) {
            if (auto* cs = canvasSceneById(layer.canvasSceneId)) {
                if (cs->surface())
                    if (SkCanvas* canvas = frameSegmentCanvas())
                        at.draw(canvas, cs->surface()->makeImageSnapshot(), layer);
            }
        } else if (layer.type == UILayer::Scene3D) {
#if BRO_WITH_3D
            scene::SceneGraph* graph = nullptr;
            for (auto& sg : sceneGraphs_) {
                if (sg.graph && (layer.elementId == 0 || sg.elementId == layer.elementId)) {
                    graph = sg.graph.get();
                    break;
                }
            }
            if (!graph || !graph->renderer().hasMeshContent()) continue;
            const render::LayerImage out = graph->renderer().outputImage();
            if (out) addImage(out.image, out.layout, out.width, out.height, layer);
#endif
        } else if (layer.type == UILayer::WebGL) {
            webgl::WebGL2RenderingContext* wctx = nullptr;
            for (auto& entry : webglEntries_) {
                if (entry.context && entry.element && entry.element->nodeId() == layer.elementId) {
                    wctx = entry.context.get();
                    break;
                }
            }
            if (!wctx || wctx->colorImage() == VK_NULL_HANDLE) continue;
            // The canvas's recorded work must be submitted before the
            // presenter's submission samples it (queue order does the rest).
            wctx->flush();
            addImage(wctx->colorImage(), wctx->colorLayout(), static_cast<uint32_t>(wctx->canvasWidth()),
                     static_cast<uint32_t>(wctx->canvasHeight()), layer);
        }
    }
}

static render::PresentPixels layerOf(SkSurface* surface) {
    return surface ? render::VulkanPresenter::surfaceLayer(surface) : render::PresentPixels{};
}

// The frame as a PresentFrame: the first segment below, then each GPU image
// with the segment composited after it above.
render::PresentFrame Engine::describeCompositedFrame() {
    render::PresentFrame frame;
    if (!frameSegments_.empty()) frame.below = layerOf(frameSegments_[0].get());
    frame.images = std::move(frameImages_);
    for (size_t i = 0; i < frame.images.size(); ++i) {
        const size_t segment = i + 1;
        if (segment < frameSegmentUsed_.size() && frameSegmentUsed_[segment])
            frame.images[i].above = layerOf(frameSegments_[segment].get());
    }
    frameImages_.clear();
    frameSegmentUsed_.assign(1, true);
    return frame;
}

void Engine::presentCurrentFrame() {
    const render::PresentFrame frame = describeCompositedFrame();
    if (vulkanPresenter_ && !vulkanPresenter_->isHeadless()) {
        if (!vulkanPresenter_->present(frame)) LOG_ERROR("Engine: presenting the frame failed");
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
    const render::PresentFrame frame = describeCompositedFrame();
    if (frame.hasImages() && vulkanPresenter_ && vulkanPresenter_->isHeadless()) {
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        if (vulkanPresenter_->present(frame) && vulkanPresenter_->readbackPixels(pixels, w, h))
            return pixels;
        LOG_ERROR("Engine: GPU frame composite/readback failed");
        return {};
    }
    SkPixmap pm;
    if (frameSegments_.empty() || !frameSegments_[0] || !frameSegments_[0]->peekPixels(&pm)) return {};
    return render::pixmapToRgba(pm);
}

} // namespace bro::engine
