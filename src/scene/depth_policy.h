#pragma once

// Depth-buffer policy for the 3D scene renderer.
//
// A world that runs continuously from a metre underfoot to a planet seen from
// orbit spans ~7 orders of magnitude of depth. A conventional depth mapping
// cannot carry that: near maps to 0 and the float exponent range clusters
// where the depth distribution is already dense, so precision collapses to a
// near/far RATIO of a few thousand before z-fighting sets in.
//
// Reversed-Z fixes it. With near and far swapped in the [0,1] clip range,
// floating-point's exponent clustering near 0 lines up with the depth
// distribution's clustering near the far plane, and the two errors cancel to
// something very close to uniform precision. Combined with a 32F depth buffer
// it comfortably carries 1 m to 10^6 m.
//
// The policy is one process-wide flag, read once: BRO_DISABLE_REVERSED_Z=1
// selects the conventional mapping, so the fallback stays testable. Every
// consumer asks reversedZ() — the camera projections here, the culling planes,
// and on the GPU side the clear value, the depth compare ops, the MSAA depth
// resolve and the shaders' depth reconstructions (scene/vulkan/scene_vk_depth.h,
// which hands the flag to every pipeline as specialization constant 0).
//
// Clip space is always Vulkan's [0,1] depth range; only the direction flips.
// The Y flip from bromath's y-up clip space to Vulkan's y-down one is not part
// of these matrices — it happens once, in SceneView (scene/vulkan/scene_view.h).

#include <bromath/frustum.h>
#include <bromath/mat.h>

#include <cmath>

namespace bro::scene {

/// True when camera depth is reversed (near = 1, far = 0). Decided on first
/// call from BRO_DISABLE_REVERSED_Z and fixed for the life of the process:
/// projections, clears, compare ops and compiled pipelines must all agree.
bool reversedZ();

/// The depth value that means "infinitely far" under the policy.
inline float depthClearFar() { return reversedZ() ? 0.0f : 1.0f; }

/// Right-handed perspective for a CAMERA: [0,1] clip depth, reversed when the
/// policy is (z = near maps to 1, z = far to 0), otherwise near 0 / far 1.
///
/// The far plane is kept finite deliberately. An infinite projection is the
/// textbook companion to reversed-Z and costs nothing in depth precision, but
/// it also removes the far culling plane — camera.far would quietly stop
/// meaning anything, and every frustum cull would keep geometry the caller
/// asked to drop.
inline bromath::Mat4 makePerspective(float fovY, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    bromath::Mat4 m;
    for (int i = 0; i < 16; ++i) m.data[i] = 0.0f;
    m.at(0, 0) = f / aspect;
    m.at(1, 1) = f;
    m.at(3, 2) = -1.0f;
    if (reversedZ()) {
        m.at(2, 2) = znear / (zfar - znear);
        m.at(2, 3) = (zfar * znear) / (zfar - znear);
    } else {
        m.at(2, 2) = zfar / (znear - zfar);
        m.at(2, 3) = (zfar * znear) / (znear - zfar);
    }
    return m;
}

/// Right-handed perspective in [0,1] that is NEVER reversed — near 0, far 1.
///
/// For shadow maps: they are their own depth targets with their own near/far,
/// fitted tightly per cascade, so they never had a precision problem worth
/// reversing for. Keeping them conventional keeps the depth bias signs and the
/// LESS_OR_EQUAL compare sampler the same under either camera policy.
inline bromath::Mat4 makePerspectiveZeroToOne(float fovY, float aspect,
                                              float znear, float zfar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    bromath::Mat4 m;
    for (int i = 0; i < 16; ++i) m.data[i] = 0.0f;
    m.at(0, 0) = f / aspect;
    m.at(1, 1) = f;
    m.at(2, 2) = zfar / (znear - zfar);
    m.at(2, 3) = (zfar * znear) / (znear - zfar);
    m.at(3, 2) = -1.0f;
    return m;
}

/// Right-handed orthographic for a CAMERA — [0,1], reversed with the policy.
///
/// Orthographic depth is linear, so reversing buys it no precision. It
/// follows the policy anyway because the depth clear value and the compare op
/// belong to the pass, not the projection: an ortho camera that mapped near to
/// 0 in a reversed pass would clear the buffer to "nearest" and then reject
/// every fragment.
inline bromath::Mat4 makeOrtho(float l, float r, float b, float t,
                               float znear, float zfar) {
    bromath::Mat4 m;
    for (int i = 0; i < 16; ++i) m.data[i] = 0.0f;
    m.at(0, 0) = 2.0f / (r - l);
    m.at(1, 1) = 2.0f / (t - b);
    m.at(0, 3) = -(r + l) / (r - l);
    m.at(1, 3) = -(t + b) / (t - b);
    m.at(3, 3) = 1.0f;
    if (reversedZ()) {
        m.at(2, 2) = 1.0f / (zfar - znear);
        m.at(2, 3) = zfar / (zfar - znear);
    } else {
        m.at(2, 2) = -1.0f / (zfar - znear);
        m.at(2, 3) = -znear / (zfar - znear);
    }
    return m;
}

/// Right-handed orthographic in [0,1] that is NEVER reversed — the shadow
/// cascade builder (see makePerspectiveZeroToOne).
inline bromath::Mat4 makeOrthoZeroToOne(float l, float r, float b, float t,
                                        float znear, float zfar) {
    bromath::Mat4 m;
    for (int i = 0; i < 16; ++i) m.data[i] = 0.0f;
    m.at(0, 0) = 2.0f / (r - l);
    m.at(1, 1) = 2.0f / (t - b);
    m.at(2, 2) = -1.0f / (zfar - znear);
    m.at(0, 3) = -(r + l) / (r - l);
    m.at(1, 3) = -(t + b) / (t - b);
    m.at(2, 3) = -znear / (zfar - znear);
    m.at(3, 3) = 1.0f;
    return m;
}

/// The six culling planes of a view-projection built by any function above.
/// Gribb-Hartmann derives the depth pair from the clip inequality 0 <= z <= w,
/// which holds for both directions, so one extraction serves either policy.
inline bromath::Frustum makeFrustum(const bromath::Mat4& vp) {
    return bromath::ffromViewProj(vp, /*zeroToOneDepth=*/true);
}

}  // namespace bro::scene
