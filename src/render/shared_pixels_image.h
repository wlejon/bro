#pragma once

// SharedPixels (renderer.h) as Skia images: a raster SkImage over the pixels
// themselves (no copy; the image holds the owner), and the draw both Skia
// renderers make of a source rect.

#include "render/renderer.h"

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

class SkCanvas;

namespace bro::render {

// Null when `px` has no pixels.
sk_sp<SkImage> makeSharedPixelsImage(const SharedPixels& px);

void drawSharedPixelsImage(SkCanvas* canvas, const sk_sp<SkImage>& image, float sx, float sy, float sw, float sh,
                           float x, float y, float w, float h);

} // namespace bro::render
