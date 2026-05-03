#pragma once

namespace bro::dom { class Element; }
namespace bro::render { class Renderer; }

namespace bro::engine {

/// Devtools-style box-model overlay over the selected element. Draws:
///   - margin / border / padding / content donut rects
///   - per-side numeric labels on margin and padding strips
///   - viewport-spanning guide lines from each border edge
///   - a header label near the element with `<tag.class#id>  W × H px`
///
/// Coordinates of `el->layoutBox()` are relative to its layout parent's
/// content box; this routine accumulates ancestor offsets up to the document
/// element, then translates by (insetLeft, insetTop - scrollY).
///
/// `viewportW` / `viewportH` are the *app* viewport (i.e. the
/// content-inset-reduced area). Guides extend across this space only.
void drawInspectorHighlight(render::Renderer* renderer, dom::Element* el,
                            float scrollY, int insetLeft, int insetTop,
                            int viewportW, int viewportH);

} // namespace bro::engine
