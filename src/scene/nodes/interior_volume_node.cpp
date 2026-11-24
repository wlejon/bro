#include "scene/nodes/interior_volume_node.h"

#include <bromath/mat.h>

#include <algorithm>
#include <cmath>

namespace bro::scene {

namespace {

float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

float smoothstep01(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

InteriorVolumeNode::InteriorVolumeNode(const std::string& name)
    : SceneNode(name) {
    setScale(10.0f, 10.0f, 10.0f);
}

void InteriorVolumeNode::setSkyVisibility(float v) {
    skyVisibility_ = clamp01(v);
}

void InteriorVolumeNode::setSunVisibility(float v) {
    sunVisibility_ = clamp01(v);
}

InteriorEnclosureSample evaluateInteriorEnclosures(const std::vector<const InteriorVolumeNode*>& sortedVolumes,
                                                   const bromath::Vec3& worldPos,
                                                   bool exposureOnly) {
    float accSky = 0.0f;
    float accSun = 0.0f;
    bromath::Vec3 accAmb{0.0f, 0.0f, 0.0f};
    float accW = 0.0f;

    for (const InteriorVolumeNode* v : sortedVolumes) {
        if (!v) continue;
        if (exposureOnly && !v->affectExposure()) continue;

        const bromath::Mat4& wm = v->worldMatrix();
        const bromath::Mat4 inv = bromath::minverse(wm);
        const bromath::Vec3 lp{
            inv.at(0, 0) * worldPos.x + inv.at(0, 1) * worldPos.y + inv.at(0, 2) * worldPos.z + inv.at(0, 3),
            inv.at(1, 0) * worldPos.x + inv.at(1, 1) * worldPos.y + inv.at(1, 2) * worldPos.z + inv.at(1, 3),
            inv.at(2, 0) * worldPos.x + inv.at(2, 1) * worldPos.y + inv.at(2, 2) * worldPos.z + inv.at(2, 3),
        };
        auto axisLen = [&](int col) {
            return std::sqrt(wm.at(0, col) * wm.at(0, col) +
                             wm.at(1, col) * wm.at(1, col) +
                             wm.at(2, col) * wm.at(2, col));
        };
        const float ex = (0.5f - std::fabs(lp.x)) * axisLen(0);
        const float ey = (0.5f - std::fabs(lp.y)) * axisLen(1);
        const float ez = (0.5f - std::fabs(lp.z)) * axisLen(2);
        const float dmin = std::min({ex, ey, ez});
        if (dmin <= 0.0f) continue;

        const float blend = v->blendDistance();
        const float w = blend > 0.0f ? smoothstep01(0.0f, blend, dmin) : 1.0f;
        const float contrib = w * (1.0f - accW);
        accSky += v->skyVisibility() * contrib;
        accSun += v->sunVisibility() * contrib;
        const bromath::Vec3 ac = v->ambientColor();
        // (Daylight through the sides' windows, as lighting.bk's eval_enclosure takes it.)
        float through = 1.0f;
        if (v->windowDepth() > 0.0f)
            through = 1.0f + v->windowShare() * (2.0f * std::exp(-std::max(std::min(ex, ez), 0.0f) / v->windowDepth()) - 1.0f);
        const float ai = v->ambientIntensity() * through;
        accAmb = accAmb + bromath::Vec3{ac.x * ai * contrib, ac.y * ai * contrib, ac.z * ai * contrib};
        accW += contrib;
        if (accW >= 0.9999f) {
            accW = 1.0f;
            break;
        }
    }

    InteriorEnclosureSample out;
    out.skyVisibility = accSky + (1.0f - accW);
    out.sunVisibility = accSun + (1.0f - accW);
    out.ambient = accAmb;
    out.weight = accW;
    return out;
}

}  // namespace bro::scene
