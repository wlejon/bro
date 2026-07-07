#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <include/core/SkMatrix.h>
#include <include/core/SkRect.h>

namespace bro::dom { class Element; }

// Shared SVG parsing helpers used by both the geometry path
// (svg_geometry.cpp, getBoundingClientRect) and the native paint traversal
// (svg_paint.cpp). All operate on the live DOM subtree of an <svg> element.
namespace bro::layout::svgcommon {

// Lowercase a string (ASCII).
std::string lower(std::string_view s);

// Attribute lookup: try the exact spelling first (JS setAttribute stores the
// name verbatim), then the lowercased one (gumbo lowercases at parse time).
const std::string& attrOf(const dom::Element* el, const char* name);

// Parse an attribute as a float, returning `fallback` when absent/unparsable.
float attrFloat(const dom::Element* el, const char* name, float fallback = 0.0f);

// Whitespace/comma separated float list (viewBox, polygon points).
std::vector<float> parseNumberList(const std::string& s);

// Elements that never render (defs/gradients/clip/mask/marker/pattern/filter
// primitives/metadata/…). `tag` must be lowercased.
bool isNonRendered(const std::string& tag);

// SVG `transform` attribute list → matrix (operations compose left-to-right).
SkMatrix parseTransformList(const std::string& s);

// viewBox + preserveAspectRatio → viewport transform (user units → CSS px
// within the viewport). Identity when there is no usable viewBox.
SkMatrix viewportMatrix(float vpW, float vpH,
                        const std::string& viewBox, const std::string& par);

// The element's own transform contribution when mapping its geometry into the
// parent's user space: the `transform` attribute, plus x/y translation and the
// nested viewport transform for inner <svg> elements. `tag` must be lowercased.
SkMatrix ownTransform(const dom::Element* el, const std::string& tag);

// Depth-first search for a descendant (or self) with a matching id attribute.
const dom::Element* findById(const dom::Element* root, const std::string& id);

// Convert an SVG basic shape (rect/circle/ellipse/line/polygon/polyline) or a
// <path> into path data ('d') in the element's local user units. Returns "" if
// `el` is not a basic shape or has no geometry. For <path> returns the `d`
// attribute directly.
std::string shapeToPathData(const dom::Element* el);

// Tight fill bounding box of a path 'd' string (geometry only, no stroke).
// Returns false when the path has no points.
bool pathBounds(const std::string& d, SkRect& out);

} // namespace bro::layout::svgcommon
