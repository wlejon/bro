#pragma once

// SharedPixels (renderer.h) as Skia images: a raster SkImage over the pixels
// themselves (no copy; the image holds the owner), and the draw both Skia
// renderers make of a source rect.

#include "render/renderer.h"

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>
#include <include/core/SkSamplingOptions.h>

class SkCanvas;

namespace bro::render {

// Null when `px` has no pixels.
sk_sp<SkImage> makeSharedPixelsImage(const SharedPixels& px);

// The Skia sampling for `sampling`. Smooth is bilinear, and with `mipmaps`
// trilinear (a level-0 lookup where the image is magnified, an averaged
// smaller level where it is minified — what keeps a photo shrunk to fit from
// shimmering); the image must then carry mips (a texture made Mipmapped) or
// be a raster image Skia can build them for. Pixelated is nearest.
SkSamplingOptions imageSamplingOptions(ImageSampling sampling, bool mipmaps);

// Draw the source rect of `image` into (x, y, w, h). A source rect inside the
// image samples strictly within it (abutting pieces meet without seams); the
// whole image samples freely, which is what lets mipmaps apply.
void drawSharedPixelsImage(SkCanvas* canvas, const sk_sp<SkImage>& image, float sx, float sy, float sw, float sh,
                           float x, float y, float w, float h,
                           ImageSampling sampling = ImageSampling::Smooth, bool mipmaps = false);

} // namespace bro::render
