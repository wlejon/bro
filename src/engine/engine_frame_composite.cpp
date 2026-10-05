// Frame composition: the CPU composite of a frame's UI layers, and its
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
#include <include/core/SkData.h>
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
    void draw(SkCanvas* canvas, const sk_sp<SkImage>& img, const UILayer& l) const {
        if (!img) return;
        canvas->save();
        if (l.clipW >= 0.0f && l.clipH >= 0.0f) {
            canvas->clipRect(SkRect::MakeXYWH(l.clipX * sx, (l.clipY + oy) * sy, l.clipW * sx, l.clipH * sy),
                             SkClipOp::kIntersect, true);
        }
        canvas->drawImageRect(img, dst(l), SkSamplingOptions(SkFilterMode::kLinear));
        canvas->restore();
    }
};

sk_sp<SkImage> rgbaImage(std::vector<uint8_t>&& px, int w, int h) {
    if (px.empty() || w <= 0 || h <= 0) return nullptr;
    SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    return SkImages::RasterFromData(info, SkData::MakeWithCopy(px.data(), px.size()),
                                    static_cast<size_t>(w) * 4);
}

} // namespace

// Start the next GPU frame: waits for the frame slot about to be reused (not
// the device) and runs the destructors deferred until it finished.
void Engine::beginGpuFrame() {
    if (vulkanContext_) vulkanContext_->frames().beginFrame();
}

// Size and clear this frame's composite surface; forget last frame's GPU base layer.
void Engine::beginFrameComposite() {
    int fbW = deviceScale_.drawableW > 0 ? deviceScale_.drawableW : viewportWidth_;
    int fbH = deviceScale_.drawableH > 0 ? deviceScale_.drawableH : viewportHeight_;
    if (!frameCompositeSurface_ || frameCompositeW_ != fbW || frameCompositeH_ != fbH) {
        frameCompositeSurface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(fbW, fbH));
        frameAboveSurface_.reset();
        frameCompositeW_ = fbW;
        frameCompositeH_ = fbH;
    }
    if (frameCompositeSurface_) frameCompositeSurface_->getCanvas()->clear(SK_ColorTRANSPARENT);
    frameAboveActive_ = false;
    pendingVkImage_ = VK_NULL_HANDLE;
    pendingVkImageLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    pendingVkImageW_ = pendingVkImageH_ = 0;
}

void Engine::compositeLayers(const std::vector<UILayer>& layers, uint32_t /*targetFBO*/,
                             int offsetY, int /*layerW*/, int /*layerH*/) {
    if (layers.empty() || !frameCompositeSurface_) return;

    const int fbW = frameCompositeW_, fbH = frameCompositeH_;
    SkCanvas* canvas = frameAboveActive_ ? frameAboveSurface_->getCanvas()
                                         : frameCompositeSurface_->getCanvas();
    LayerPlacement at;
    at.sx = static_cast<float>(fbW) / static_cast<float>(viewportWidth_ > 0 ? viewportWidth_ : 1);
    at.sy = static_cast<float>(fbH) / static_cast<float>(viewportHeight_ > 0 ? viewportHeight_ : 1);
    at.oy = static_cast<float>(offsetY);

    // A layer covering the whole frame can be presented straight from its GPU
    // image instead of being read back into the composite: the layers so far
    // stay below it, and the ones after it composite into the surface above.
    auto coversFrame = [&](const UILayer& l) {
        SkRect r = at.dst(l);
        return r.left() <= 1.0f && r.top() <= 1.0f &&
               std::abs(r.width() - fbW) <= 2.0f && std::abs(r.height() - fbH) <= 2.0f;
    };
    auto claimFrameImage = [&](VkImage image, VkImageLayout layout, uint32_t w, uint32_t h) {
        pendingVkImage_ = image;
        pendingVkImageLayout_ = layout;
        pendingVkImageW_ = w;
        pendingVkImageH_ = h;
        if (!frameAboveSurface_)
            frameAboveSurface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(fbW, fbH));
        if (!frameAboveSurface_) return;
        frameAboveSurface_->getCanvas()->clear(SK_ColorTRANSPARENT);
        frameAboveActive_ = true;
        canvas = frameAboveSurface_->getCanvas();
    };

    for (const auto& layer : layers) {
        if (layer.type == UILayer::HTML) {
            if (layer.surface) {
                if (auto img = layer.surface->makeImageSnapshot())
                    canvas->drawImage(img, 0.0f, at.oy * at.sy);
            }
        } else if (layer.type == UILayer::Iframe) {
            if (auto* d = iframeDocById(layer.canvasSceneId)) at.draw(canvas, d->published.get(), layer);
        } else if (layer.type == UILayer::Canvas) {
            if (auto* cs = canvasSceneById(layer.canvasSceneId)) {
                if (cs->surface()) at.draw(canvas, cs->surface()->makeImageSnapshot(), layer);
            }
        } else if (layer.type == UILayer::Scene3D) {
#if BRO_WITH_3D
            scene::SceneGraph* graph = nullptr;
            for (auto& sg : sceneGraphs_) {
                if (sg.graph && (layer.texture == 0 || sg.elementId == layer.texture)) {
                    graph = sg.graph.get();
                    break;
                }
            }
            if (!graph || !graph->renderer().hasMeshContent()) continue;
            const render::LayerImage out = graph->renderer().outputImage();
            if (coversFrame(layer) && pendingVkImage_ == VK_NULL_HANDLE && vulkanPresenter_ && out) {
                claimFrameImage(out.image, out.layout, out.width, out.height);
            } else {
                int w = 0, h = 0;
                auto px = graph->readTonemapPixelsRGBA(w, h);
                at.draw(canvas, rgbaImage(std::move(px), w, h), layer);
            }
#endif
        } else if (layer.type == UILayer::WebGL) {
            webgl::WebGL2RenderingContext* wctx = nullptr;
            for (auto& entry : webglEntries_) {
                if (entry.context && entry.element && entry.element->nodeId() == layer.texture) {
                    wctx = entry.context.get();
                    break;
                }
            }
            if (!wctx) continue;
            // The canvas's recorded work must be submitted before the
            // presenter's submission samples it (queue order does the rest).
            wctx->flush();
            if (coversFrame(layer) && pendingVkImage_ == VK_NULL_HANDLE && vulkanPresenter_ &&
                wctx->vkColorImage() != VK_NULL_HANDLE) {
                claimFrameImage(wctx->vkColorImage(), wctx->vkColorLayout(),
                                static_cast<uint32_t>(wctx->canvasWidth()),
                                static_cast<uint32_t>(wctx->canvasHeight()));
            } else {
                std::vector<uint8_t> px;
                if (wctx->readCanvasPixels(px))
                    at.draw(canvas, rgbaImage(std::move(px), wctx->canvasWidth(), wctx->canvasHeight()), layer);
            }
        }
    }
}

static render::PresentPixels layerOf(SkSurface* surface) {
    return surface ? render::VulkanPresenter::surfaceLayer(surface) : render::PresentPixels{};
}

// The frame as a PresentFrame: the CPU composite below the pending GPU image
// (if a layer claimed one), and the layers composited after it above.
render::PresentFrame Engine::describeCompositedFrame() {
    render::PresentFrame frame;
    frame.below = layerOf(frameCompositeSurface_.get());
    if (pendingVkImage_ != VK_NULL_HANDLE && pendingVkImageW_ > 0 && pendingVkImageH_ > 0) {
        frame.image = pendingVkImage_;
        frame.imageLayout = pendingVkImageLayout_ != VK_IMAGE_LAYOUT_UNDEFINED
                                ? pendingVkImageLayout_
                                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        frame.imageWidth = pendingVkImageW_;
        frame.imageHeight = pendingVkImageH_;
        if (frameAboveActive_) frame.above = layerOf(frameAboveSurface_.get());
    }
    pendingVkImage_ = VK_NULL_HANDLE;
    frameAboveActive_ = false;
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
// CPU composite is read straight from it; one with a GPU base layer is
// composited by the presenter and read back, in one submission and one wait.
std::vector<uint8_t> Engine::readCompositedFrame() {
    const render::PresentFrame frame = describeCompositedFrame();
    if (frame.hasImage() && vulkanPresenter_ && vulkanPresenter_->isHeadless()) {
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        if (vulkanPresenter_->present(frame) && vulkanPresenter_->readbackPixels(pixels, w, h))
            return pixels;
        LOG_ERROR("Engine: GPU frame composite/readback failed");
        return {};
    }
    SkPixmap pm;
    if (!frameCompositeSurface_ || !frameCompositeSurface_->peekPixels(&pm)) return {};
    return render::pixmapToRgba(pm);
}

} // namespace bro::engine
