#pragma once

#include "render/renderer.h"
#include <include/core/SkBlendMode.h>
#include <include/core/SkRefCnt.h>
#include <span>

class SkCanvas;
class SkImageFilter;
class SkRRect;
class SkPaint;
struct SkRect;

namespace bro::render {

// Map a CSS mix-blend-mode to the matching SkBlendMode. Unknown/Normal → kSrcOver.
SkBlendMode toSkBlendMode(BlendMode mode);

// Build a Skia SkImageFilter chain from a list of CSS filter descriptors.
// Returns nullptr for an empty input. Used by SkiaRenderer and RasterRenderer
// to implement Renderer::saveLayerWithFilter without leaking Skia types into
// the public Renderer interface or DrawTraversal.
sk_sp<SkImageFilter> BuildSkImageFilterChain(std::span<const CssFilterParams> filters);

// The Filter Effects color matrices CSS and SVG share, row-major 4x5 with
// offsets in 0..1: saturate(s) (CSS saturate(), feColorMatrix type=saturate),
// a hue rotation by `degrees` (CSS hue-rotate(), type=hueRotate), and
// luminanceToAlpha.
void SaturateMatrix(float s, float out[20]);
void HueRotateMatrix(float degrees, float out[20]);
void LuminanceToAlphaMatrix(float out[20]);

// Build the SkImageFilter graph of an SVG <filter> (Renderer::
// saveLayerWithSvgFilter): primitives in document order, inputs by index,
// each in its color-interpolation-filters space, cropped to its subregion,
// the result in sRGB cropped to `region`.
sk_sp<SkImageFilter> BuildSvgFilterGraph(std::span<const SvgFilterPrimitive> prims,
                                         std::span<const float> data, const SkRect& region);

// The paint of an SVG mask layer (Renderer::saveLayerMask): destination-in,
// through luma-to-alpha for MaskMode::Luminance.
void SetMaskLayerPaint(MaskMode mode, SkPaint& paint);

// Renderer::drawBackdropFilter on a Skia canvas: an empty layer over `clip`
// initialised from the filtered backdrop, restored straight back at `opacity`.
void DrawSkBackdropFilter(SkCanvas* canvas, std::span<const CssFilterParams> filters, const SkRRect& clip,
                          float opacity);

} // namespace bro::render
