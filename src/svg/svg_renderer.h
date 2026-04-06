#pragma once

#include "render/renderer.h"

namespace bro::dom { class Element; }

namespace bro::svg {

/// Render an <svg> DOM element at position (x, y) within a box of size (w, h).
/// Uses Skia's SkSVGDOM module for full SVG support (gradients, filters, clips, etc.).
void renderSvg(render::Renderer* renderer, dom::Element* svgElement,
               float x, float y, float w, float h);

} // namespace bro::svg
