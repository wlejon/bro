#pragma once

#include "scene/scene_node.h"
#include <bromath/color.h>
#include <cstdint>
#include <vector>

namespace bro::scene {

// Color is bromath::Color (linear-float RGBA). Header refs must spell out
// the full namespace; .cpp may `using bromath::Color;` for brevity.

/// A renderable 2D shape (rect, circle, polygon) with fill and stroke.
class ShapeNode : public SceneNode {
public:
    enum class Shape : uint8_t { Rect, RoundRect, Circle, Ellipse, Polygon, Line };

    explicit ShapeNode(const std::string& name = "");

    Type type() const override { return Type::Shape; }
    void onRender(SceneGraph& graph) override;

    // --- Shape configuration ---

    Shape shape() const { return shape_; }
    void setShape(Shape s) { shape_ = s; }

    /// Dimensions for Rect/RoundRect (width, height).
    void setSize(float w, float h) { width_ = w; height_ = h; }
    float width() const { return width_; }
    float height() const { return height_; }

    /// Corner radius for RoundRect.
    void setCornerRadius(float r) { cornerRadius_ = r; }
    float cornerRadius() const { return cornerRadius_; }

    /// Radius for Circle.
    void setRadius(float r) { radius_ = r; }
    float radius() const { return radius_; }

    /// Radii for Ellipse.
    void setRadii(float rx, float ry) { radiusX_ = rx; radiusY_ = ry; }
    float radiusX() const { return radiusX_; }
    float radiusY() const { return radiusY_; }

    /// Points for Polygon (pairs of x,y).
    void setPoints(const std::vector<float>& pts) { points_ = pts; }
    const std::vector<float>& points() const { return points_; }

    /// Line endpoints (x1, y1, x2, y2) relative to node origin.
    void setLineEndpoints(float x1, float y1, float x2, float y2) {
        lineX1_ = x1; lineY1_ = y1; lineX2_ = x2; lineY2_ = y2;
    }

    // --- Appearance ---

    void setFillColor(bromath::Color c) { fillColor_ = c; hasFill_ = true; }
    bromath::Color fillColor() const { return fillColor_; }
    bool hasFill() const { return hasFill_; }
    void setHasFill(bool v) { hasFill_ = v; }

    void setStrokeColor(bromath::Color c) { strokeColor_ = c; hasStroke_ = true; }
    bromath::Color strokeColor() const { return strokeColor_; }
    bool hasStroke() const { return hasStroke_; }
    void setHasStroke(bool v) { hasStroke_ = v; }

    void setStrokeWidth(float w) { strokeWidth_ = w; if (w > 0.0f) hasStroke_ = true; }
    float strokeWidth() const { return strokeWidth_; }

    /// Anchor point (0..1). Default (0.5, 0.5) = center-origin.
    void setAnchor(float ax, float ay) { anchorX_ = ax; anchorY_ = ay; }
    float anchorX() const { return anchorX_; }
    float anchorY() const { return anchorY_; }

private:
    Shape shape_ = Shape::Rect;
    float width_ = 50, height_ = 50;
    float cornerRadius_ = 0;
    float radius_ = 25;
    float radiusX_ = 25, radiusY_ = 25;
    float lineX1_ = 0, lineY1_ = 0, lineX2_ = 50, lineY2_ = 0;
    std::vector<float> points_;

    bromath::Color fillColor_{1.0f, 1.0f, 1.0f, 1.0f};
    bromath::Color strokeColor_{0.0f, 0.0f, 0.0f, 1.0f};
    float strokeWidth_ = 1.0f;
    bool hasFill_ = true;
    bool hasStroke_ = false;

    float anchorX_ = 0.5f, anchorY_ = 0.5f;
};

} // namespace bro::scene
