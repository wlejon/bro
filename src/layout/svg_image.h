#pragma once

#include <cstddef>

namespace bro::render { class Renderer; struct DecodedImage; }

namespace bro::layout {

// SVG documents shown as images: `<img src="*.svg">`, CSS url() images, an
// <image> inside an SVG, and every consumer that rasterizes one
// (render/svg_image.h). The markup is parsed as XML into a detached
// bro::dom document (the DOMParser path), its styles cascaded, and its <svg>
// painted by the same traversal as inline SVG (layout/svg_paint.h), scaled
// into the box: the root's own width/height give way to the box, and a
// document without a viewBox gets one spanning its intrinsic size. As in
// browsers, the document loads nothing: its <image>s show data: URIs only.

// Paint a decoded SVG image (its `svgMarkup`) into (x, y, w, h). The parsed
// document is kept per image (by its pixels id), so repaints do not reparse.
void paintSvgImage(render::Renderer* r, const render::DecodedImage& img,
                   float x, float y, float w, float h);

// Install the painter render's SVG rasterizer calls (render::setSvgPainter).
// Once at engine startup, before any image decodes.
void installSvgImagePainter();

} // namespace bro::layout
