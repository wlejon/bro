#include "scene/shape_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

namespace bro::scene {

ShapeNode::ShapeNode(const std::string& name) : SceneNode(name) {}

void ShapeNode::onRender(SceneGraph& graph) {
    auto* cs = graph.canvasScene();
    if (!cs) return;

    const auto& wm = worldMatrix();

    // Apply world transform via canvas save/restore
    cs->save();
    // Canvas transform: setTransform(a, b, c, d, e, f) maps to our Mat3
    cs->setTransform(wm.a, wm.c, wm.b, wm.d, wm.tx, wm.ty);

    // Compute anchor offset
    float ax, ay;

    switch (shape_) {
    case Shape::Rect:
    case Shape::RoundRect: {
        ax = -width_ * anchorX_;
        ay = -height_ * anchorY_;
        if (hasFill_) {
            cs->setFillColor(fillColor_.r, fillColor_.g, fillColor_.b, fillColor_.a);
            cs->fillRect(ax, ay, width_, height_);
        }
        if (hasStroke_) {
            cs->setStrokeColor(strokeColor_.r, strokeColor_.g, strokeColor_.b, strokeColor_.a);
            cs->setLineWidth(strokeWidth_);
            cs->strokeRect(ax, ay, width_, height_);
        }
        break;
    }
    case Shape::Circle: {
        if (hasFill_) {
            cs->setFillColor(fillColor_.r, fillColor_.g, fillColor_.b, fillColor_.a);
            cs->beginPath();
            cs->arc(0, 0, radius_, 0, 6.283185307f, false);
            cs->fill();
        }
        if (hasStroke_) {
            cs->setStrokeColor(strokeColor_.r, strokeColor_.g, strokeColor_.b, strokeColor_.a);
            cs->setLineWidth(strokeWidth_);
            cs->beginPath();
            cs->arc(0, 0, radius_, 0, 6.283185307f, false);
            cs->stroke();
        }
        break;
    }
    case Shape::Ellipse: {
        if (hasFill_) {
            cs->setFillColor(fillColor_.r, fillColor_.g, fillColor_.b, fillColor_.a);
            cs->beginPath();
            cs->ellipse(0, 0, radiusX_, radiusY_, 0, 0, 6.283185307f, false);
            cs->fill();
        }
        if (hasStroke_) {
            cs->setStrokeColor(strokeColor_.r, strokeColor_.g, strokeColor_.b, strokeColor_.a);
            cs->setLineWidth(strokeWidth_);
            cs->beginPath();
            cs->ellipse(0, 0, radiusX_, radiusY_, 0, 0, 6.283185307f, false);
            cs->stroke();
        }
        break;
    }
    case Shape::Polygon: {
        if (points_.size() < 4) break; // need at least 2 points
        cs->beginPath();
        cs->moveTo(points_[0], points_[1]);
        for (size_t i = 2; i + 1 < points_.size(); i += 2) {
            cs->lineTo(points_[i], points_[i + 1]);
        }
        cs->closePath();
        if (hasFill_) {
            cs->setFillColor(fillColor_.r, fillColor_.g, fillColor_.b, fillColor_.a);
            cs->fill();
        }
        if (hasStroke_) {
            cs->setStrokeColor(strokeColor_.r, strokeColor_.g, strokeColor_.b, strokeColor_.a);
            cs->setLineWidth(strokeWidth_);
            cs->stroke();
        }
        break;
    }
    case Shape::Line: {
        if (hasStroke_ || hasFill_) {
            auto c = hasStroke_ ? strokeColor_ : fillColor_;
            cs->setStrokeColor(c.r, c.g, c.b, c.a);
            cs->setLineWidth(strokeWidth_);
            cs->beginPath();
            cs->moveTo(lineX1_, lineY1_);
            cs->lineTo(lineX2_, lineY2_);
            cs->stroke();
        }
        break;
    }
    }

    cs->restore();
}

} // namespace bro::scene
