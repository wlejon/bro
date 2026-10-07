#pragma once

#include <string>

class SkCanvas;

namespace bro::render {

/// Draw a vector software cursor at (x, y) with the specified CSS shape name
/// ("default", "pointer", "text", "crosshair", "move", "ew-resize", "ns-resize", etc.).
/// If shape is "none" or empty, nothing is drawn.
void drawSoftwareCursor(SkCanvas* canvas, float x, float y, const std::string& shape, float scale = 1.0f);

} // namespace bro::render
