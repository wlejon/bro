#pragma once

#include "engine/gizmo.h"
#include <cmath>

namespace bro::engine {

constexpr float kGizmoPi = 3.14159265358979323846f;
constexpr float kGizmoViewRingScale = 1.25f;
constexpr float kGizmoPlaneAlpha = 0.55f;

inline bool isPlaneAxis(GizmoAxis a) {
    return a == GizmoAxis::XY || a == GizmoAxis::YZ || a == GizmoAxis::XZ;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline float vlen_(const bromath::Vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

inline bromath::Vec3 vnorm_(const bromath::Vec3& v) {
    float l = vlen_(v);
    return l > 1e-9f ? bromath::Vec3(v.x / l, v.y / l, v.z / l) : bromath::Vec3(1.0f, 0.0f, 0.0f);
}

} // namespace bro::engine
