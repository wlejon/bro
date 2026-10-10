// SkiaRenderer's surfaces: the raster frame surface, the compositing layer
// surfaces (GPU with a SkiaGpu, CPU raster without), and pixel readback for
// screenshots. The engine composites the layers and VulkanPresenter shows
// them.

#include "render/skia_backend.h"
#include "broimage/encode.h"
#include "render/pixel_convert.h"
#include "render/shared_pixels_image.h"
#include "util/log.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/gpu/ganesh/GrRecordingContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>
#include <include/private/chromium/GrDeferredDisplayList.h>
#include <include/private/chromium/GrDeferredDisplayListRecorder.h>
#include <include/private/chromium/GrSurfaceCharacterization.h>

#include <algorithm>

namespace bro::render {

void SkiaRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    }

    canvas_ = surface_ ? surface_->getCanvas() : nullptr;
    if (canvas_) enterCanvas();

    imageCache_.beginFrame();
    ++imageFrame_;
    evictGpuImages(/*all=*/false);
    inFrame_ = true;
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    endRecording();
    canvas_ = nullptr;
    inFrame_ = false;

    if (gpu_ && !touched_.empty()) {
        SkiaGpu::Lock lock = gpu_->lock();
        std::vector<SkSurface*> surfaces;
        surfaces.reserve(touched_.size());
        for (auto& s : touched_) surfaces.push_back(s.get());
        gpu_->finish(surfaces);
        touched_.clear();
    }
    auto callbacks = std::move(afterSubmit_);
    afterSubmit_.clear();
    for (auto& fn : callbacks) fn();
}

void SkiaRenderer::afterSubmit(std::function<void()> fn) {
    afterSubmit_.push_back(std::move(fn));
}

sk_sp<SkSurface> SkiaRenderer::switchSurface(sk_sp<SkSurface> newSurface) {
    if (canvas_) canvas_->restore();
    endRecording();

    auto prev = std::move(surface_);
    surface_ = std::move(newSurface);
    canvas_ = nullptr;
    if (surface_ && surface_->recordingContext()) {
        // A GPU surface: record what is drawn on it without the context lock.
        GrSurfaceCharacterization characterization;
        bool characterized = false;
        {
            SkiaGpu::Lock lock = gpu_->lock();
            characterized = surface_->characterize(&characterization);
        }
        if (characterized) {
            recorder_ = std::make_unique<GrDeferredDisplayListRecorder>(characterization);
            canvas_ = recorder_->getCanvas();
        }
        if (!canvas_) LOG_ERROR("SkiaRenderer: cannot record for a GPU surface; its layer stays blank");
        if (inFrame_ && std::find(touched_.begin(), touched_.end(), surface_) == touched_.end())
            touched_.push_back(surface_);
    } else if (surface_) {
        canvas_ = surface_->getCanvas();
    }
    if (canvas_) enterCanvas();

    return prev;
}

void SkiaRenderer::endRecording() {
    if (!recorder_) return;
    sk_sp<GrDeferredDisplayList> ddl = recorder_->detach();
    SkiaGpu::Lock lock = gpu_->lock();
    recorder_.reset();
    if (ddl && !skgpu::ganesh::DrawDDL(surface_.get(), ddl))
        LOG_ERROR("SkiaRenderer: a recorded layer does not match its surface");
    ddl.reset();
    // Outside a frame there is no endFrame to finish what was drawn.
    if (!inFrame_) gpu_->finish(surface_.get());
}

sk_sp<SkImage> SkiaRenderer::gpuImage(uint64_t id, const sk_sp<SkImage>& source) {
    auto it = gpuImages_.find(id);
    if (it != gpuImages_.end() && it->second.source == source) {
        it->second.lastFrame = imageFrame_;
        return it->second.texture;
    }
    SkiaGpu::Lock lock = gpu_->lock();
    // Mipmapped: an image drawn smaller than it is samples a smaller level
    // rather than aliasing (imageSamplingOptions).
    sk_sp<SkImage> texture = SkImages::TextureFromImage(gpu_->context(), source, skgpu::Mipmapped::kYes,
                                                        skgpu::Budgeted::kYes);
    if (!texture) return source;
    gpuImages_[id] = GpuImage{source, texture, imageFrame_};
    return texture;
}

void SkiaRenderer::drawSharedPixels(const SharedPixels& px, float sx, float sy, float sw, float sh,
                                    float x, float y, float w, float h, ImageSampling sampling) {
    if (!canvas_ || !px.rgba || px.id == 0) return;
    SharedImage& e = sharedImages_[px.id];
    if (e.rgba != px.rgba || !e.raster) {
        e = SharedImage{};
        e.raster = makeSharedPixelsImage(px);
        e.rgba = px.rgba;
    }
    e.lastFrame = imageFrame_;
    if (!e.raster) return;
    sk_sp<SkImage> image = e.raster;
    const bool wantMips = sampling == ImageSampling::Smooth;
    if (recorder_) {
        // A recorded (GPU) list samples a texture, uploaded once per id —
        // with mips the first time it is drawn smoothed.
        if (!e.texture || (wantMips && !e.mipmapped)) {
            SkiaGpu::Lock lock = gpu_->lock();
            sk_sp<SkImage> tex = SkImages::TextureFromImage(
                gpu_->context(), e.raster, wantMips ? skgpu::Mipmapped::kYes : skgpu::Mipmapped::kNo,
                skgpu::Budgeted::kYes);
            if (tex) {
                e.texture = std::move(tex);
                e.mipmapped = wantMips;
            }
        }
        if (e.texture) image = e.texture;
    }
    drawSharedPixelsImage(canvas_, image, sx, sy, sw, sh, x, y, w, h, sampling,
                          /*mipmaps=*/wantMips && (!recorder_ || e.mipmapped));
}

void SkiaRenderer::evictGpuImages(bool all) {
    // As long as the decoded image cache keeps its entries (and a little
    // longer: a replaced source just stops matching).
    constexpr uint64_t kEvictAfterFrames = 60;
    if (gpuImages_.empty() && sharedImages_.empty()) return;
    SkiaGpu::Lock lock;
    if (gpu_) lock = gpu_->lock();
    for (auto it = gpuImages_.begin(); it != gpuImages_.end();) {
        if (all || it->second.lastFrame + kEvictAfterFrames < imageFrame_) it = gpuImages_.erase(it);
        else ++it;
    }
    for (auto it = sharedImages_.begin(); it != sharedImages_.end();) {
        if (all || it->second.lastFrame + kEvictAfterFrames < imageFrame_) it = sharedImages_.erase(it);
        else ++it;
    }
}

void SkiaRenderer::enterCanvas() {
    if (!canvas_) return;
    canvas_->restoreToCount(1);
    canvas_->resetMatrix();
    canvas_->clear(SK_ColorTRANSPARENT);
    if (deviceScale_ != 1.0f) canvas_->scale(deviceScale_, deviceScale_);
    canvas_->save();
}

SkiaRenderer::LayerSurface SkiaRenderer::createLayerSurface(int width, int height) {
    if (gpu_) return gpu_->makeSurface(width, height);
    return {SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height)), nullptr};
}

void SkiaRenderer::fitLayerSurface(LayerSurface& surf, int width, int height) {
    if (surf.surface && surf.surface->width() == width && surf.surface->height() == height) return;
    surf = createLayerSurface(width, height);
}

bool SkiaRenderer::saveScreenshot(const std::string& path) {
    if (!surface_) return false;

    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return false;

    const int w = pixmap.width(), h = pixmap.height();
    std::vector<uint8_t> rgba = pixmapToRgba(pixmap);
    if (rgba.empty()) return false;

    return broimage::encode_png_file(path, rgba.data(), w, h, 4);
}

std::vector<uint8_t> SkiaRenderer::capturePixels() {
    if (!surface_) return {};

    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return {};

    return pixmapToRgba(pixmap);
}

} // namespace bro::render
