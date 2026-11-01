#pragma once

// Internal to the SVG paint traversal (svg_paint.cpp, svg_paint_server.cpp,
// svg_filter.cpp): the state one paint of an <svg> subtree carries, and the
// entry points the pieces call into each other through.

#include "layout/svg_paint.h"
#include "render/renderer.h"

#include <include/core/SkPath.h>
#include <include/core/SkRect.h>

#include <string>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::layout {

struct SvgPaintCtx {
    render::Renderer* r = nullptr;
    const dom::Element* root = nullptr;       // id scope for url(#id) / href
    const SvgImageResolver* images = nullptr; // <image> href → pixels
    int depth = 0;                            // reference nesting (use, pattern, mask, marker)
    float vpW = 0, vpH = 0;                   // the nearest viewport, user units (percentages)
    static constexpr int kMaxDepth = 12;
};

// Paint one element (and its subtree) in the current user space.
void svgPaintNode(SvgPaintCtx& ctx, const dom::Element* el);

// Bounds of an element's geometry in its own user space (its own transform
// excluded): what objectBoundingBox units resolve against.
SkRect svgObjectBounds(SvgPaintCtx& ctx, const dom::Element* el);

// ---- paint servers (svg_paint_server.cpp) ----------------------------------

// A resolved fill or stroke.
struct SvgPaint {
    enum class Kind { None, Solid, Gradient, Pattern } kind = Kind::None;
    render::GradientPaint paint;            // Solid / Gradient, opacity folded in
    std::vector<render::ColorStop> stops;   // Gradient
    const dom::Element* pattern = nullptr;  // Pattern
    float opacity = 1.0f;                   // Pattern: fill-/stroke-opacity
};

// Resolve a fill/stroke value against `bbox` (objectBoundingBox units).
SvgPaint resolveSvgPaint(const SvgPaintCtx& ctx, const std::string& value, float opacity,
                         const SkRect& bbox, const bromath::Color& currentColor);

// Fill the area `area` (user space) with a <pattern>: tiles of the pattern's
// content, clipped to the area. `bbox` is the painted element's bounds.
void paintPatternArea(SvgPaintCtx& ctx, const dom::Element* pattern, const SkPath& area,
                      const SkRect& bbox, float opacity);

// Paint `path` with a resolved fill and stroke (any mix of solid, gradient and
// pattern), the way a shape or glyph outline is painted. `d`, when given, is
// `path` as path data, recorded as is (re-serializing a parsed path turns its
// arcs into many quads, which cost far more to draw). With `d`, `path` may
// be left empty when neither paint is a pattern.
void paintSvgGeometry(SvgPaintCtx& ctx, const SkPath& path, render::PathFillRule rule,
                      const SvgPaint& fill, const SvgPaint& stroke,
                      const render::StrokeStyle& ss, const std::vector<float>& dash,
                      const SkRect& bbox, const std::string* d = nullptr);

// ---- filters (svg_filter.cpp) ----------------------------------------------

// The `filter` of `el` as an SVG filter graph, resolved to `el`'s user space.
// `region` receives the filter region. False when the element has no
// (resolvable) url() filter.
bool buildSvgFilter(const SvgPaintCtx& ctx, const dom::Element* el, const SkRect& bbox,
                    std::vector<render::SvgFilterPrimitive>& prims, std::vector<float>& data,
                    SkRect& region);

} // namespace bro::layout
