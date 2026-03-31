#pragma once

#include "svg/svg_types.h"

namespace bro::dom { class Element; }

namespace bro::svg {

/// Parse an SVG tree from a bro::dom <svg> element and its children.
SvgRoot parseSvgTree(bro::dom::Element* svgElement);

} // namespace bro::svg
