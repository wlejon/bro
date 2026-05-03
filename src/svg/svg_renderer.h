#pragma once

#include "render/renderer.h"

namespace bro::dom { class Element; }

namespace bro::svg {

/// Render an <svg> DOM element at position (x, y) within a box of size (w, h).
/// Uses Skia's SkSVGDOM module for full SVG support (gradients, filters, clips, etc.).
void renderSvg(render::Renderer* renderer, dom::Element* svgElement,
               float x, float y, float w, float h);

/// Render raw SVG markup bytes (e.g. from a data:image/svg+xml URL) at
/// (x, y) sized to (w, h). Same Skia SVG pipeline as renderSvg().
void renderSvgMarkup(render::Renderer* renderer,
                     const char* data, size_t len,
                     float x, float y, float w, float h);

} // namespace bro::svg
