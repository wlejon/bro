#pragma once

#include "dom/element_geometry.h"

namespace bro::dom { class Element; }
namespace bro::render { class Renderer; }

namespace bro::layout {

/// getBoundingClientRect() geometry for elements inside an <svg> subtree.
///
/// SVG children are not part of the CSS layout tree (the <svg> element is a
/// replaced element rendered by the Skia SVG module), so they have no layout
/// boxes. This computes their client rect the way Chromium does: the fill
/// (object) bounding box of the element's geometry — tight curve bounds for
/// paths, unions of child boxes for containers — mapped through the SVG
/// `transform` attribute chain and the viewBox/preserveAspectRatio viewport
/// transform into CSS pixels, then offset by the <svg> root's content-box
/// position (projected through any CSS transforms on its ancestors). Stroke
/// is NOT included, matching Chromium's getBoundingClientRect for SVG.
///
/// Non-rendered elements (defs and everything inside them, gradients, stops,
/// clipPath, mask, marker, pattern, symbol, metadata, title, desc, filter
/// primitives) report all-zeros, matching Chromium.
///
/// Returns true if `el` is inside an <svg> subtree and `out` was filled
/// (possibly with zeros); false if `el` is not an SVG child (including the
/// <svg> root element itself), in which case the normal layout-box path
/// applies.
///
/// `renderer` (optional) supplies font measurement for <text>/<tspan> bounds;
/// when null those elements report no geometry (all-zeros).
bool svgChildBoundingClientRect(const dom::Element* el, dom::AbsoluteRect& out,
                                render::Renderer* renderer = nullptr);

} // namespace bro::layout
