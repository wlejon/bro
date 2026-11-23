#pragma once

#include "scene/scene_node.h"

#include <bromath/vec.h>

#include <vector>

namespace bro::scene {

/// Axis-aligned / oriented interior enclosure volume (unit box [-0.5, 0.5]^3
/// in local space, matching ReflectionProbeNode and DecalNode: node `scale` IS
/// the world-space [width, height, depth] extent).
///
/// Attenuates outdoor sky SH irradiance and optionally outdoor sun inside the
/// volume while adding warm interior ambient fill. Couples to GPU incident
/// auto-exposure when `affectExposure` is true.
class InteriorVolumeNode : public SceneNode {
public:
    explicit InteriorVolumeNode(const std::string& name = "");

    Type type() const override { return Type::InteriorVolume; }

    float skyVisibility() const { return skyVisibility_; }
    void setSkyVisibility(float v);

    float sunVisibility() const { return sunVisibility_; }
    void setSunVisibility(float v);

    const bromath::Vec3& ambientColor() const { return ambientColor_; }
    void setAmbientColor(const bromath::Vec3& c) { ambientColor_ = c; }
    void setAmbientColor(float r, float g, float b) { ambientColor_ = {r, g, b}; }

    float ambientIntensity() const { return ambientIntensity_; }
    void setAmbientIntensity(float v) { ambientIntensity_ = v < 0.0f ? 0.0f : v; }

    /// Inward fade margin from box faces in world units. 0 = hard boundary.
    float blendDistance() const { return blendDistance_; }
    void setBlendDistance(float d) { blendDistance_ = d < 0.0f ? 0.0f : d; }

    int priority() const { return priority_; }
    void setPriority(int p) { priority_ = p; }

    bool affectExposure() const { return affectExposure_; }
    void setAffectExposure(bool on) { affectExposure_ = on; }

private:
    float skyVisibility_ = 0.05f;
    float sunVisibility_ = 1.0f;
    bromath::Vec3 ambientColor_{1.0f, 0.90f, 0.76f};
    float ambientIntensity_ = 0.0f;
    float blendDistance_ = 1.0f;
    int priority_ = 0;
    bool affectExposure_ = true;
};

struct InteriorEnclosureSample {
    float skyVisibility = 1.0f;
    float sunVisibility = 1.0f;
    bromath::Vec3 ambient{0.0f, 0.0f, 0.0f};
    float weight = 0.0f;
};

/// Evaluates sorted interior enclosure volumes (highest priority / smallest
/// volume first) at `worldPos`, matching GPU `eval_enclosure` in local_lighting.bk.
InteriorEnclosureSample evaluateInteriorEnclosures(const std::vector<const InteriorVolumeNode*>& sortedVolumes,
                                                   const bromath::Vec3& worldPos,
                                                   bool exposureOnly = false);

}  // namespace bro::scene
