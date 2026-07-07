#pragma once

namespace bro::dom { class Element; }
namespace bro::render { class Renderer; }

namespace bro::layout {

// True when the whole <svg> subtree can be rendered by the native paint
// traversal below. Returns false when it contains a feature still handled by
// the Skia SkSVGDOM fallback (text, filters, masks, patterns, markers,
// symbols, raster <image>, or a computed filter/mask/clip-path).
bool svgSubtreeNativelySupported(const dom::Element* svgRoot);

// Paint an <svg> subtree natively into the content rect (x, y, w, h) by walking
// the DOM, resolving each element's cascaded SVG paint (fill/stroke, gradients,
// currentColor, stroke styling, fill-rule, opacity) and emitting Renderer
// primitives. Runs at record time on the main thread (live DOM); the emitted
// commands replay DOM-free.
void paintSvgSubtree(render::Renderer* r, const dom::Element* svgRoot,
                     float x, float y, float w, float h);

} // namespace bro::layout
