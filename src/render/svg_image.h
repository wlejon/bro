#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bro::render {

class Renderer;

// SVG documents as images (`<img src="*.svg">`, CSS url(), bro.image, the
// canvas/WebGL consumers that need pixels). Parsing and painting an SVG is the
// layout module's (layout/svg_image.h: the document is a bro::dom tree,
// cascaded and painted by the same traversal as inline <svg>), which sits
// above render; it installs the painter here at startup, so render's image
// paths can rasterize without depending on it.

/// Paint SVG markup scaled into the box (0, 0, w, h) of `r`. False when the
/// markup is not an SVG document.
using SvgPainter = bool (*)(Renderer* r, const char* data, size_t len, float w, float h);
void setSvgPainter(SvgPainter painter);

/// Does this byte range look like an SVG document? A cheap sniff of the leading
/// bytes, for deciding whether a file a bitmap decoder rejected is worth
/// handing to the SVG parser.
bool looksLikeSvg(const char* data, size_t len);

/// The document's intrinsic pixel size: its `width`/`height` attributes when it
/// declares them, else the extent of its `viewBox`. Either output is left at 0
/// when the markup says nothing — which is a real answer, not a failure: an SVG
/// with only a viewBox has an intrinsic *ratio* and no intrinsic size, and it is
/// the caller's business what concrete size to give it.
///
/// This is the one place that reads those attributes, so layout's intrinsic
/// size, the paint path's box, and the rasterizer cannot disagree about how
/// big a given icon is.
void svgIntrinsicSize(const char* data, size_t len, float& outW, float& outH);

/// The viewBox of the outer <svg> tag; false when it has none.
bool svgViewBox(const char* data, size_t len, float out[4]);

/// Rasterize SVG markup into a straight-alpha (unpremultiplied) RGBA8888
/// buffer — the shape every other decoded image in bro has. `reqW`/`reqH` are
/// the pixel dimensions; 0 takes them from the intrinsic size (else CSS's
/// 300x150 default object size). False when no painter is installed, the
/// markup does not parse, or the size is unusable. Any thread.
bool rasterizeSvgMarkup(const char* data, size_t len, int reqW, int reqH,
                        int& outW, int& outH, std::vector<uint8_t>& outPixels);

} // namespace bro::render
