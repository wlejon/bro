#pragma once

#include <functional>
#include <memory>
#include <string>

namespace bro::dom { class Element; }
namespace bro::render { class Renderer; struct DecodedImage; }

namespace bro::layout {

// The pixels an <image href> names, or null (not loaded yet, broken, or not
// allowed): inline <svg> resolves through the page's image store, an SVG
// document shown as an image through its own data: URIs only.
using SvgImageResolver =
    std::function<std::shared_ptr<const render::DecodedImage>(const std::string& href)>;

// Paint an <svg> subtree into the content rect (x, y, w, h) by walking the
// DOM, resolving each element's cascaded SVG paint and emitting Renderer
// primitives: shapes with solid, gradient and pattern fills and strokes,
// markers, text (glyphs, or outlines when stroked, pattern- or gradient-
// filled, or on a textPath), <use> of elements and symbols, nested <svg>,
// <image>, clip-path, mask, SVG filters and CSS filter functions, opacity.
// This is bro's only SVG renderer: inline <svg> and SVG documents shown as
// images (layout/svg_image.h) both paint through it. Runs at record time
// (live DOM); the emitted commands replay DOM-free.
void paintSvgSubtree(render::Renderer* r, const dom::Element* svgRoot,
                     float x, float y, float w, float h,
                     const SvgImageResolver& images = {});

} // namespace bro::layout
