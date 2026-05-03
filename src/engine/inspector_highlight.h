#pragma once

namespace bro::dom { class Element; }
namespace bro::render { class Renderer; }

namespace bro::engine {

/// Devtools-style box-model overlay over the selected element. Drawn after
/// the app document is rasterized, so the colored rects sit on top of the
/// rendered content.
///
/// Coordinates of `el->layoutBox()` are relative to its layout parent's
/// content box; this routine accumulates ancestor offsets up to the document
/// element, then translates by (insetLeft, insetTop - scrollY).
void drawInspectorHighlight(render::Renderer* renderer, dom::Element* el,
                            float scrollY, int insetLeft, int insetTop);

} // namespace bro::engine
