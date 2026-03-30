#pragma once

#include "render/renderer.h"
#include <string>
#include <vector>
#include <optional>

namespace bro::svg {

struct SvgStyle {
    render::Color fill{0, 0, 0, 255};     // default: black
    render::Color stroke{0, 0, 0, 0};     // default: none
    float strokeWidth = 1.0f;
    float opacity = 1.0f;
    bool hasFill = true;
    bool hasStroke = false;
};

enum class SvgNodeType {
    Rect, Circle, Ellipse, Line, Polyline, Polygon, Path, Text, Group
};

struct SvgNode {
    SvgNodeType type;
    SvgStyle style;
    std::vector<SvgNode> children; // for Group

    // Rect
    float x = 0, y = 0, width = 0, height = 0, rx = 0, ry = 0;

    // Circle
    float cx = 0, cy = 0, r = 0;

    // Ellipse (reuses cx, cy)
    // float rx, ry — reuses rx, ry from Rect

    // Line
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;

    // Path
    std::string pathData;

    // Polyline / Polygon
    std::vector<render::PointF> points;

    // Text
    std::string textContent;
    float fontSize = 16.0f;
    std::string fontFamily = "Arial";
};

struct SvgRoot {
    float width = 300;
    float height = 150;
    bool hasViewBox = false;
    float viewBoxX = 0, viewBoxY = 0, viewBoxW = 0, viewBoxH = 0;
    std::vector<SvgNode> children;
};

} // namespace bro::svg
