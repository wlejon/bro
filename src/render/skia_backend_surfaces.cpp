// SkiaRenderer's surfaces: the raster frame surface, the compositing layer
// surfaces, and pixel readback for screenshots. Skia draws on the CPU; the
// engine composites the results and VulkanPresenter shows them.

#include "render/skia_backend.h"
#include "broimage/encode.h"
#include "render/pixel_convert.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>

namespace bro::render {

void SkiaRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
    }

    canvas_ = surface_ ? surface_->getCanvas() : nullptr;
    if (canvas_) enterCanvas();

    imageCache_.beginFrame();
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;
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

SkiaRenderer::LayerSurface SkiaRenderer::createLayerSurface(int width, int height) {
    return {SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height))};
}

void SkiaRenderer::fitLayerSurface(LayerSurface& surf, int width, int height) {
    if (surf.surface && surf.surface->width() == width && surf.surface->height() == height) return;
    surf.surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
}

void SkiaRenderer::releaseLayerSurface(LayerSurface& surf) {
    surf.surface.reset();
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
