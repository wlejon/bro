#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class SkCanvas;

namespace bro::svg {

/// Rasterize SVG markup into a straight-alpha (unpremultiplied) RGBA8888
/// buffer — the shape every other decoded image in bro has, so an `<img
/// src="…​.svg">` is an ordinary image everywhere downstream rather than a
/// second kind of picture the draw path has to know about.
///
/// `reqW`/`reqH` are the pixel dimensions to rasterize at; pass 0 for either to
/// take it from the document's own intrinsic size (its `width`/`height`
/// attributes, else its `viewBox`). Returns false — leaving the outputs
/// untouched — when the markup does not parse or resolves to no usable size.
bool rasterizeSvgMarkup(const char* data, size_t len,
                        int reqW, int reqH,
                        int& outW, int& outH,
                        std::vector<uint8_t>& outPixels);

/// Does this byte range look like an SVG document? A cheap sniff of the leading
/// non-whitespace bytes, for deciding whether a file a bitmap decoder rejected
/// is worth handing to the SVG parser.
bool looksLikeSvg(const char* data, size_t len);

/// The document's intrinsic pixel size: its `width`/`height` attributes when it
/// declares them, else the extent of its `viewBox`. Either output is left at 0
/// when the markup says nothing — which is a real answer, not a failure: an SVG
/// with only a viewBox has an intrinsic *ratio* and no intrinsic size, and it is
/// the caller's business what concrete size to give it.
///
/// This is the one place that reads those attributes, so layout's intrinsic
/// size, the paint path's cache entry, and the rasterizer cannot disagree about
/// how big a given icon is.
void svgIntrinsicSize(const char* data, size_t len, float& outW, float& outH);

/// Render raw SVG markup bytes onto the given Skia canvas at (x, y) sized to
/// (w, h). Used by Skia-backed Renderer implementations to honor a recorded
/// drawSvgMarkup command. Backends call this with their owned canvas; the
/// recording layer captures the markup bytes into the command buffer arena so
/// replay can re-parse them on the raster thread.
void renderSvgMarkupToCanvas(SkCanvas* canvas,
                             const char* data, size_t len,
                             float x, float y, float w, float h);

} // namespace bro::svg
