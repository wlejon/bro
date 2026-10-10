#include "render/shared_pixels_image.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkData.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkSamplingOptions.h>

#include <memory>

namespace bro::render {

sk_sp<SkImage> makeSharedPixelsImage(const SharedPixels& px) {
    if (!px.rgba || px.width <= 0 || px.height <= 0) return nullptr;
    const size_t rowBytes = size_t(px.width) * 4;
    // The image's data releases a heap copy of the owner: the pixels live as
    // long as any SkImage (or texture upload in flight) still refers to them.
    auto* keep = new std::shared_ptr<const void>(px.owner);
    sk_sp<SkData> data = SkData::MakeWithProc(
        px.rgba, rowBytes * size_t(px.height),
        [](const void*, void* ctx) { delete static_cast<std::shared_ptr<const void>*>(ctx); }, keep);
    const SkImageInfo info = SkImageInfo::Make(px.width, px.height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    return SkImages::RasterFromData(info, std::move(data), rowBytes);
}

SkSamplingOptions imageSamplingOptions(ImageSampling sampling, bool mipmaps) {
    if (sampling == ImageSampling::Pixelated)
        return SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone);
    return SkSamplingOptions(SkFilterMode::kLinear, mipmaps ? SkMipmapMode::kLinear : SkMipmapMode::kNone);
}

void drawSharedPixelsImage(SkCanvas* canvas, const sk_sp<SkImage>& image, float sx, float sy, float sw, float sh,
                           float x, float y, float w, float h, ImageSampling sampling, bool mipmaps) {
    if (!canvas || !image || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    // Never a texel from outside a source rect that is part of the image: the
    // kitty placeholder cells of one image are drawn as abutting pieces.
    const bool whole = sx <= 0 && sy <= 0 && sx + sw >= image->width() && sy + sh >= image->height();
    canvas->drawImageRect(image, SkRect::MakeXYWH(sx, sy, sw, sh), SkRect::MakeXYWH(x, y, w, h),
                          imageSamplingOptions(sampling, mipmaps && whole), nullptr,
                          whole ? SkCanvas::kFast_SrcRectConstraint : SkCanvas::kStrict_SrcRectConstraint);
}

} // namespace bro::render
