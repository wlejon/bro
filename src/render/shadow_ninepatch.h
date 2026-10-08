#pragma once

// A blurred rounded rectangle (a box-shadow) drawn as a nine-patch of a
// cached mask. Ganesh blurs a rect, or a rounded rect whose corners are all
// the same circle, analytically on the GPU; any other rounded rect (corners
// rounded on one side only, elliptical corners) falls back to a CPU mask
// blur of the whole shape, every time it is drawn. At window-shadow sizes
// (hundreds of px, sigma 50+) that is 10 ms and more a frame, paid again on
// every re-record of the layer the shadow is in.
//
// The blurred mask of a rounded rect only varies across its corners: the
// middle of each edge is the same profile repeated. So the mask of the
// smallest rect with the same corners and the same blur, cut into nine
// pieces with the middle row and column stretched, is the mask of the large
// one exactly. Masks are cached by corner radii and sigma (device px), so a
// shadow is blurred once however often it moves or resizes.

#include <include/core/SkColor.h>

class SkCanvas;
class SkRRect;

namespace bro::render {

/// Whether Ganesh would blur `rrect` on the CPU (it is neither a rect nor a
/// rounded rect with one circular radius at every corner).
bool shadowNeedsNinePatch(const SkRRect& rrect);

/// Draws `rrect` (local coordinates) blurred by `sigma` (local units) in
/// `color` through the canvas's current matrix and clip. False, having drawn
/// nothing, when it cannot (a rotating or skewing matrix, a shape too small
/// for a nine-patch to save anything): draw it the ordinary way then.
bool drawBlurredRRectNinePatch(SkCanvas* canvas, const SkRRect& rrect, float sigma, SkColor color);

}  // namespace bro::render
