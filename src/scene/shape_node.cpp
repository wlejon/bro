#include "scene/shape_node.h"
#include "scene/scene_graph.h"
#include "canvas/canvas_scene.h"

namespace bro::scene {

// Bridge: shape colors are linear-float (bromath::Color); CanvasScene wants
// sRGB uint8 per channel. Encode at the boundary.
static inline void setFillC(canvas::CanvasScene* cs, bromath::Color c) {
    bromath::Color8 p = bromath::ctoColor8(c);
    cs->setFillColor(p.r, p.g, p.b, p.a);
}
static inline void setStrokeC(canvas::CanvasScene* cs, bromath::Color c) {
    bromath::Color8 p = bromath::ctoColor8(c);
    cs->setStrokeColor(p.r, p.g, p.b, p.a);
}

ShapeNode::ShapeNode(const std::string& name) : SceneNode(name) {}

void ShapeNode::onRender(SceneGraph& graph) {
    auto* cs = graph.canvasScene();
    if (!cs) return;

    const auto& wm = worldMatrix();

    // Apply world transform via canvas save/restore
    cs->save();
    // Extract 2D affine components from 4x4 column-major matrix:
    // Canvas setTransform(a, b, c, d, e, f) where the matrix is:
    //   | a c e |     col-major Mat4: a=m[0][0], b=m[0][1], c=m[1][0], d=m[1][1], e=m[3][0], f=m[3][1]
    //   | b d f |
    //   | 0 0 1 |
    cs->setTransform(wm.at(0, 0), wm.at(1, 0), wm.at(0, 1), wm.at(1, 1), wm.at(0, 3), wm.at(1, 3));

    // Compute anchor offset
    float ax, ay;

    switch (shape_) {
    case Shape::Rect:
    case Shape::RoundRect: {
        ax = -width_ * anchorX_;
        ay = -height_ * anchorY_;
        if (hasFill_) {
            setFillC(cs, fillColor_);
            cs->fillRect(ax, ay, width_, height_);
        }
        if (hasStroke_) {
            setStrokeC(cs, strokeColor_);
            cs->setLineWidth(strokeWidth_);
            cs->strokeRect(ax, ay, width_, height_);
        }
        break;
    }
    case Shape::Circle: {
        if (hasFill_) {
            setFillC(cs, fillColor_);
            cs->beginPath();
            cs->arc(0, 0, radius_, 0, 6.283185307f, false);
            cs->fill();
        }
        if (hasStroke_) {
            setStrokeC(cs, strokeColor_);
            cs->setLineWidth(strokeWidth_);
            cs->beginPath();
            cs->arc(0, 0, radius_, 0, 6.283185307f, false);
            cs->stroke();
        }
        break;
    }
    case Shape::Ellipse: {
        if (hasFill_) {
            setFillC(cs, fillColor_);
            cs->beginPath();
            cs->ellipse(0, 0, radiusX_, radiusY_, 0, 0, 6.283185307f, false);
            cs->fill();
        }
        if (hasStroke_) {
            setStrokeC(cs, strokeColor_);
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
            setFillC(cs, fillColor_);
            cs->fill();
        }
        if (hasStroke_) {
            setStrokeC(cs, strokeColor_);
            cs->setLineWidth(strokeWidth_);
            cs->stroke();
        }
        break;
    }
    case Shape::Line: {
        if (hasStroke_ || hasFill_) {
            auto c = hasStroke_ ? strokeColor_ : fillColor_;
            setStrokeC(cs, c);
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
