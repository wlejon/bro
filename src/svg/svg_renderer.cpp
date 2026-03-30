#include "svg/svg_renderer.h"

#include <algorithm>
#include <cmath>

namespace bro::svg {

namespace {

void renderNode(render::Renderer* renderer, const SvgNode& node) {
    render::Color fill = node.style.hasFill ? node.style.fill : render::Color{0, 0, 0, 0};
    render::Color stroke = node.style.hasStroke ? node.style.stroke : render::Color{0, 0, 0, 0};
    float sw = node.style.strokeWidth;

    switch (node.type) {
    case SvgNodeType::Rect:
        if (node.rx > 0 || node.ry > 0) {
            if (fill.a > 0)
                renderer->fillRoundRect(node.x, node.y, node.width, node.height,
                                        node.rx, node.ry, fill);
            if (stroke.a > 0 && sw > 0)
                renderer->drawRoundRect(node.x, node.y, node.width, node.height,
                                        node.rx, node.ry, stroke);
        } else {
            if (fill.a > 0)
                renderer->fillRect(node.x, node.y, node.width, node.height, fill);
            if (stroke.a > 0 && sw > 0)
                renderer->drawRect(node.x, node.y, node.width, node.height, stroke);
        }
        break;

    case SvgNodeType::Circle:
        renderer->drawCircle(node.cx, node.cy, node.r, fill, stroke, sw);
        break;

    case SvgNodeType::Ellipse:
        renderer->drawEllipse(node.cx, node.cy, node.rx, node.ry, fill, stroke, sw);
        break;

    case SvgNodeType::Line:
        if (stroke.a > 0 && sw > 0)
            renderer->drawLine(node.x1, node.y1, node.x2, node.y2, stroke, sw);
        break;

    case SvgNodeType::Polyline:
        if (!node.points.empty())
            renderer->drawPolyline(node.points, stroke, sw);
        break;

    case SvgNodeType::Polygon:
        if (!node.points.empty())
            renderer->drawPolygon(node.points, fill, stroke, sw);
        break;

    case SvgNodeType::Path:
        if (!node.pathData.empty())
            renderer->drawPath(node.pathData, fill, stroke, sw);
        break;

    case SvgNodeType::Text: {
        if (fill.a > 0 && !node.textContent.empty()) {
            uint64_t font = renderer->createFont(node.fontFamily, node.fontSize, 400, false);
            renderer->drawText(node.textContent, node.x, node.y, font, fill);
            renderer->deleteFont(font);
        }
        break;
    }

    case SvgNodeType::Group:
        renderer->save();
        for (auto& child : node.children)
            renderNode(renderer, child);
        renderer->restore();
        break;
    }
}

} // anonymous namespace

void renderSvg(render::Renderer* renderer, const SvgRoot& root,
               float x, float y, float w, float h) {
    renderer->save();
    renderer->translate(x, y);

    // Clip to the SVG box
    renderer->setClip(0, 0, w, h);

    if (root.hasViewBox && root.viewBoxW > 0 && root.viewBoxH > 0) {
        // preserveAspectRatio="xMidYMid meet" (default)
        float sx = w / root.viewBoxW;
        float sy = h / root.viewBoxH;
        float scale = std::min(sx, sy);

        float tx = (w - root.viewBoxW * scale) / 2.0f;
        float ty = (h - root.viewBoxH * scale) / 2.0f;
        renderer->translate(tx, ty);
        renderer->scale(scale, scale);
        renderer->translate(-root.viewBoxX, -root.viewBoxY);
    } else {
        // Scale intrinsic dimensions to the CSS box
        if (root.width > 0 && root.height > 0) {
            float sx = w / root.width;
            float sy = h / root.height;
            renderer->scale(sx, sy);
        }
    }

    for (auto& child : root.children)
        renderNode(renderer, child);

    renderer->resetClip();
    renderer->restore();
}

} // namespace bro::svg
