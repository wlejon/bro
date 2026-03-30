#pragma once

#include "svg/svg_types.h"
#include <litehtml.h>

namespace bro::svg {

/// Parse an SVG tree from a litehtml <svg> element and its children.
SvgRoot parseSvgTree(const litehtml::element::ptr& svgElement);

} // namespace bro::svg
