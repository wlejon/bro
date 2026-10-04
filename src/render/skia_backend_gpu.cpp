// SkiaRenderer's backend plumbing: raster UI surface, pixel readback for screenshots,
// and offscreen layer surfaces. With Vulkan as the sole graphics backend, Skia operates
// via CPU raster surfaces and presents through VulkanPresenter.

#include "render/skia_backend.h"
#include "broimage/encode.h"
#include "render/pixel_convert.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

namespace bro::render {

sk_sp<GrDirectContext> SkiaRenderer::createGrContext() {
#if defined(SK_VULKAN)
    // When Skia is built with Ganesh Vulkan enabled (SK_VULKAN),
    // GrDirectContexts::MakeVulkan can be instantiated from the active
    // VulkanDevice instance, physical device, queue, and function pointers.
    // If not enabled or unavailable, gracefully fall back to CPU raster.
#endif
    // With Vulkan as the primary graphics engine, Skia renders to CPU raster
    // surfaces and presents via VulkanPresenter or headless readback.
    return nullptr;
}

void SkiaRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_.reset();
        gpuMode_ = false;
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        textureWidth_ = width;
        textureHeight_ = height;
    }

    canvas_ = surface_ ? surface_->getCanvas() : nullptr;
    if (canvas_) enterCanvas();

    imageCache_.beginFrame();
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;
    pixelsPending_ = (surface_ != nullptr);
}

void SkiaRenderer::uploadToGPU() {
    pixelsPending_ = false;
}

sk_sp<SkSurface> SkiaRenderer::switchSurface(sk_sp<SkSurface> newSurface) {
    if (canvas_) canvas_->restore();

    auto prev = surface_;
    surface_ = std::move(newSurface);
    canvas_ = surface_ ? surface_->getCanvas() : nullptr;
    if (canvas_) enterCanvas();

    return prev;
}

void SkiaRenderer::enterCanvas() {
    if (!canvas_) return;
    canvas_->restoreToCount(1);
    canvas_->resetMatrix();
    canvas_->clear(SK_ColorTRANSPARENT);
    if (deviceScale_ != 1.0f) canvas_->scale(deviceScale_, deviceScale_);
    canvas_->save();
}

SkiaRenderer::GPUSurface SkiaRenderer::createGPUSurface(int width, int height) {
    GPUSurface result;
    result.surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    return result;
}

void SkiaRenderer::rewrapGPUSurface(GPUSurface& surf, int width, int height) {
    surf.surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
}

void SkiaRenderer::destroyGPUSurface(GPUSurface& surf) {
    surf.surface.reset();
    surf.fbo = 0;
    surf.texture = 0;
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
