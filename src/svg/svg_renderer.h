#pragma once

#include <cstddef>

class SkCanvas;

namespace bro::svg {

/// Render raw SVG markup bytes onto the given Skia canvas at (x, y) sized to
/// (w, h). Used by Skia-backed Renderer implementations to honor a recorded
/// drawSvgMarkup command. Backends call this with their owned canvas; the
/// recording layer captures the markup bytes into the command buffer arena so
/// replay can re-parse them on the raster thread.
void renderSvgMarkupToCanvas(SkCanvas* canvas,
                             const char* data, size_t len,
                             float x, float y, float w, float h);

} // namespace bro::svg
