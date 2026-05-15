#pragma once

#include "scene/scene_node.h"

namespace bro::scene {

/// A light source in the scene graph. LightNodes do not produce geometry;
/// SceneGraph collects every visible LightNode each frame and uploads a
/// compact array into the mesh shader for forward PBR lighting.
///
/// Three types:
///   Directional — infinite-distance light with only a direction
///                 (sun, moon). Uses `direction`.
///   Point       — omni-directional emitter at the node's world position.
///                 Uses `range` for smooth distance falloff.
///   Spot        — directional cone from the node's world position. Uses
///                 `direction`, `range`, and the inner/outer cone angles
///                 for smooth angular falloff.
///
/// `intensity` is linear radiance multiplier (pre-tonemap). Sensible
/// defaults: directional ~3.0 (sun), point ~20 (candle-bright room
/// accent), spot ~40 (stage spot).
class LightNode : public SceneNode {
public:
    enum class Kind : uint8_t { Directional, Point, Spot };

    explicit LightNode(const std::string& name = "");

    Type type() const override { return Type::Light; }

    Kind kind() const { return kind_; }
    void setKind(Kind k) { kind_ = k; }

    /// World-space direction for Directional and Spot lights. Does not need
    /// to be unit length; normalized on upload. Ignored for Point.
    const bromath::Vec3& direction() const { return direction_; }
    void setDirection(const bromath::Vec3& d) { direction_ = d; }

    /// Linear RGB in 0-1. Multiplied by `intensity` at shade time.
    const bromath::Vec3& color() const { return color_; }
    void setColor(const bromath::Vec3& c) { color_ = c; }
    void setColor(float r, float g, float b) { color_ = {r, g, b}; }

    float intensity() const { return intensity_; }
    void setIntensity(float i) { intensity_ = i; }

    /// Distance cutoff for Point and Spot lights, in world units. Fragments
    /// beyond `range` receive zero contribution; attenuation smoothly falls
    /// to zero in the outer portion of the range. Ignored for Directional.
    float range() const { return range_; }
    void setRange(float r) { range_ = r; }

    /// Spot cone angles in radians (half-angle from central axis).
    /// innerAngle: fully-lit cone. outerAngle: outer edge of falloff (total
    /// cone). Must satisfy 0 <= innerAngle <= outerAngle <= pi/2.
    float innerAngle() const { return innerAngle_; }
    float outerAngle() const { return outerAngle_; }
    void setInnerAngle(float a) { innerAngle_ = a; }
    void setOuterAngle(float a) { outerAngle_ = a; }
    void setCone(float inner, float outer) { innerAngle_ = inner; outerAngle_ = outer; }

    bool castsShadow() const { return castsShadow_; }
    void setCastsShadow(bool b) { castsShadow_ = b; }

    /// Constant depth bias added to the shadow comparison reference value
    /// (in light-clip [0,1] depth space). Tweak up if acne appears, down if
    /// peter-panning. Sensible default ~5e-4 for directional, slightly more
    /// for spot/point. Negative values are allowed.
    float shadowBias() const { return shadowBias_; }
    void setShadowBias(float b) { shadowBias_ = b; }

    /// World-space distance to push shadow-receiving fragments along their
    /// normal before sampling the shadow map. Cheap fix for self-shadow
    /// acne on curved surfaces; default 0.03.
    float shadowNormalBias() const { return shadowNormalBias_; }
    void setShadowNormalBias(float b) { shadowNormalBias_ = b; }

    /// Number of cascades for directional CSM. Clamped to [1, 4]; ignored
    /// for non-directional lights. 1 = single map (no cascading); higher
    /// gives sharper near-camera shadows at the cost of one atlas tile each.
    int cascadeCount() const { return cascadeCount_; }
    void setCascadeCount(int n) { cascadeCount_ = (n < 1) ? 1 : (n > 4 ? 4 : n); }

    /// Log/uniform split blend factor for CSM. 0 = uniform spacing (good
    /// for indoor/short-range), 1 = pure log spacing (good for outdoor /
    /// long view distance). Default 0.5 is a practical balance.
    float cascadeSplitLambda() const { return cascadeSplitLambda_; }
    void setCascadeSplitLambda(float l) { cascadeSplitLambda_ = l; }

private:
    Kind kind_ = Kind::Directional;
    bromath::Vec3 direction_{0.0f, -1.0f, 0.0f};
    bromath::Vec3 color_{1.0f, 1.0f, 1.0f};
    float intensity_ = 1.0f;
    float range_ = 10.0f;
    float innerAngle_ = 0.35f;  // ~20 deg
    float outerAngle_ = 0.52f;  // ~30 deg
    bool castsShadow_ = false;
    float shadowBias_ = 5e-4f;
    float shadowNormalBias_ = 0.03f;
    int   cascadeCount_ = 4;
    float cascadeSplitLambda_ = 0.5f;
};

} // namespace bro::scene
