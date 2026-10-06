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

void drawSharedPixelsImage(SkCanvas* canvas, const sk_sp<SkImage>& image, float sx, float sy, float sw, float sh,
                           float x, float y, float w, float h) {
    if (!canvas || !image || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
    // Linear filtering, and never a texel from outside the source rect: the
    // kitty placeholder cells of one image are drawn as abutting pieces.
    canvas->drawImageRect(image, SkRect::MakeXYWH(sx, sy, sw, sh), SkRect::MakeXYWH(x, y, w, h),
                          SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                          SkCanvas::kStrict_SrcRectConstraint);
}

} // namespace bro::render
