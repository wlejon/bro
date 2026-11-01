#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <include/core/SkMatrix.h>
#include <include/core/SkRect.h>

#include <bromath/color.h>

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

// A computed-style value, or `fallback` (the SVG property's initial value)
// when the cascade left it unset. Inside a <use> instance (UseInstanceScope)
// an inherited property the instantiated element does not set itself comes
// from the <use>, as the instance's shadow tree inherits from it rather than
// from where the template sits (usually <defs>). "Sets itself" is read as its
// value differing from its DOM parent's.
std::string styleOr(const dom::Element* el, const char* prop, const char* fallback);
float styleFloat(const dom::Element* el, const char* prop, float fallback);

// While alive, `target`'s subtree paints as an instance of `use`. Scopes nest
// (a use inside a symbol); the innermost applies first. Per thread.
class UseInstanceScope {
public:
    UseInstanceScope(const dom::Element* target, const dom::Element* use);
    ~UseInstanceScope();
    UseInstanceScope(const UseInstanceScope&) = delete;
    UseInstanceScope& operator=(const UseInstanceScope&) = delete;
};

// A color keyword/function, honoring `currentColor`. False for none or
// anything unparsable.
bool resolveColor(const std::string& v, const bromath::Color& current, bromath::Color& out);

// The element's `color`, the value currentColor names.
bromath::Color currentColorOf(const dom::Element* el);

// The id a `url(#id)` reference names (quotes and whitespace tolerated); ""
// when `v` is not one. `rest` (optional) receives what follows the `)`, the
// fallback a paint reference may carry.
std::string urlRef(const std::string& v, std::string* rest = nullptr);

// The element's href (SVG 2) or xlink:href.
const std::string& hrefOf(const dom::Element* el);

// The element an `href="#id"` names, through `root`'s ids; null otherwise.
const dom::Element* hrefTarget(const dom::Element* el, const dom::Element* root);

} // namespace bro::layout::svgcommon
