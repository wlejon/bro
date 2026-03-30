#pragma once

#include "svg/svg_types.h"
#include "render/renderer.h"

namespace bro::svg {

/// Render a parsed SVG tree at position (x, y) within a box of size (w, h).
void renderSvg(render::Renderer* renderer, const SvgRoot& root,
               float x, float y, float w, float h);

} // namespace bro::svg
