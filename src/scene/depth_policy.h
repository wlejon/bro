#pragma once

// Depth-buffer policy for the 3D scene renderer.
//
// A world that runs continuously from a metre underfoot to a planet seen from
// orbit spans ~7 orders of magnitude of depth. Conventional GL depth cannot
// carry that: it maps z to [-1,1] and then spends a float's exponent range on
// the half of NDC nobody uses, so precision collapses to a near/far RATIO of a
// few thousand before z-fighting sets in. Pushing the far plane out does not
// help — the ratio is what matters, not the distance.
//
// Reversed-Z fixes it. With clip space mapped to [0,1] and near/far swapped,
// floating-point's exponent clustering near 0 lines up with the depth
// distribution's clustering near the far plane, and the two errors cancel to
// something very close to uniform precision. Combined with a 32F depth buffer
// and an infinite far plane it comfortably carries 1 m to 10^6 m.
//
// It needs GL_ARB_clip_control (core in 4.5) and this renderer is 3.3 core, so
// availability is a runtime question. Every desktop driver we care about has
// the extension, but rather than assume, the whole policy hangs off one flag:
// when clip control is missing the renderer keeps conventional depth and the
// far field degrades to the precision we had before rather than breaking.
//
// Because the flag has to reach the shaders too (the skybox's far-plane z, the
// depth linearizers, and the depth->world reconstructions all encode the
// convention), shader sources get `#define REVERSED_Z 1` injected after their
// #version line. A shader that does not care simply ignores it.

#include <glad/gl.h>
#include <bromath/mat.h>
#include <bromath/frustum.h>

namespace bro::scene {

/// True once the renderer has successfully switched the context to
/// [0,1] clip space. Set once during GL init by initDepthPolicy(); read
/// everywhere else. Never flips mid-run — the projection matrices, the depth
/// clear value and the compiled shaders all have to agree, and they are only
/// brought into agreement at init.
extern bool gReversedZ;

/// Probe for ARB_clip_control and, if present, switch the context to
/// [0,1] clip space and set gReversedZ. Call once, after the GL loader has run
/// and before anything builds a projection matrix or clears depth.
/// Returns gReversedZ.
bool initDepthPolicy();

/// Depth comparison for ordinary "draw what is nearer" passes.
inline GLenum depthFuncCloser() { return gReversedZ ? GL_GREATER : GL_LESS; }

/// Depth comparison for "draw what is further or equal" — decal volumes test
/// against the scene depth this way to reject fragments in front of the
/// surface they are projecting onto.
inline GLenum depthFuncFartherEqual() { return gReversedZ ? GL_LEQUAL : GL_GEQUAL; }

/// The value that clears depth to "infinitely far" under the active policy.
inline GLclampd depthClearFar() { return gReversedZ ? 0.0 : 1.0; }

/// Clip-space z that parks a fullscreen quad on the far plane (the skybox
/// trick: emit z == w so the perspective divide lands exactly on the far
/// plane and the quad loses every depth test against real geometry).
inline float clipZFar() { return gReversedZ ? 0.0f : 1.0f; }

/// Internal format for depth attachments. Reversed-Z only pays off with a
/// float depth buffer — the exponent clustering it relies on does not exist in
/// a normalized integer format, where 24-bit fixed point is actually the
/// better choice under the conventional mapping.
inline GLenum depthInternalFormat() {
    return gReversedZ ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24;
}

/// Internal format for combined depth-stencil attachments (the main scene
/// target needs stencil alongside depth). Same reasoning as above: reversed-Z
/// wants the float variant. GL_DEPTH32F_STENCIL8 is core since 3.0, so it is
/// always available where clip control is.
inline GLenum depthStencilInternalFormat() {
    return gReversedZ ? GL_DEPTH32F_STENCIL8 : GL_DEPTH24_STENCIL8;
}

/// Pixel type matching depthStencilInternalFormat(). The float variant is a
/// padded two-word layout, not a packed int.
inline GLenum depthStencilType() {
    return gReversedZ ? GL_FLOAT_32_UNSIGNED_INT_24_8_REV : GL_UNSIGNED_INT_24_8;
}

/// Right-handed perspective honouring the active depth policy: [0,1] clip
/// space with near and far swapped, so z=near maps to 1 and z=far maps to 0.
///
/// The far plane is kept finite deliberately. An infinite projection is the
/// textbook companion to reversed-Z and costs nothing in depth precision, but
/// it also removes the far culling plane — camera.far would quietly stop
/// meaning anything, and every frustum cull would keep geometry the caller
/// asked to drop. Reversed-Z alone already turns the usable near/far ratio
/// from a few thousand into the millions, which is the whole point; the
/// infinite variant only buys back the last sliver.
inline bromath::Mat4 makePerspective(float fovY, float aspect, float znear, float zfar) {
    if (!gReversedZ) return bromath::mperspective(fovY, aspect, znear, zfar);

    const float f = 1.0f / std::tan(fovY * 0.5f);
    bromath::Mat4 m;
    for (int i = 0; i < 16; ++i) m.data[i] = 0.0f;
    m.at(0, 0) = f / aspect;
    m.at(1, 1) = f;
    // Standard [0,1] perspective with znear/zfar exchanged:
    //   z_ndc = znear * (zfar - dist) / (dist * (zfar - znear))
    // giving 1 at the near plane and 0 at the far plane.
    m.at(2, 2) = znear / (zfar - znear);
    m.at(2, 3) = (zfar * znear) / (zfar - znear);
    m.at(3, 2) = -1.0f;
    return m;
}

/// Right-handed perspective in [0,1] clip space but NOT reversed — near maps
/// to 0, far to 1, tested with GL_LESS.
///
/// This exists for the shadow pass. Shadow maps are their own depth targets
/// with their own near/far, fitted tightly per cascade, so they never had a
/// precision problem worth reversing for; leaving them in the conventional
/// direction keeps the depth bias signs, the GL_LEQUAL texture-compare mode
/// and the cascade fitting exactly as they were. Only the clip-space RANGE
/// has to move, because glClipControl is context-wide — every projection in
/// flight must produce [0,1] once it is on.
inline bromath::Mat4 makePerspectiveZeroToOne(float fovY, float aspect,
                                              float znear, float zfar) {
    if (!gReversedZ) return bromath::mperspective(fovY, aspect, znear, zfar);

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

/// Right-handed orthographic for a CAMERA — [0,1] and reversed, matching
/// makePerspective.
///
/// Orthographic depth is linear, so reversing buys it no precision. It
/// reverses anyway because the depth CLEAR value and the depth FUNC are
/// global per pass, not per projection: the scene pass clears to 0 and tests
/// GL_GREATER, so an ortho camera that mapped near to 0 would clear the buffer
/// to "nearest" and then reject every fragment. Whatever the main pass draws
/// has to share one convention.
inline bromath::Mat4 makeOrtho(float l, float r, float b, float t,
                               float znear, float zfar) {
    if (!gReversedZ) return bromath::mortho(l, r, b, t, znear, zfar);

    bromath::Mat4 m;
    for (int i = 0; i < 16; ++i) m.data[i] = 0.0f;
    m.at(0, 0) = 2.0f / (r - l);
    m.at(1, 1) = 2.0f / (t - b);
    // near -> 1, far -> 0.
    m.at(2, 2) = 1.0f / (zfar - znear);
    m.at(0, 3) = -(r + l) / (r - l);
    m.at(1, 3) = -(t + b) / (t - b);
    m.at(2, 3) = zfar / (zfar - znear);
    m.at(3, 3) = 1.0f;
    return m;
}

/// Right-handed orthographic in [0,1] but NOT reversed — near maps to 0, far
/// to 1, tested with GL_LESS.
///
/// This is the shadow-cascade builder. The shadow pass owns its own target,
/// its own clear and its own depth func, so it is free to keep the
/// conventional direction, which leaves the depth bias signs and the
/// GL_LEQUAL texture-compare mode exactly as they were. Only the clip-space
/// RANGE has to move, because glClipControl is context-wide.
inline bromath::Mat4 makeOrthoZeroToOne(float l, float r, float b, float t,
                                        float znear, float zfar) {
    if (!gReversedZ) return bromath::mortho(l, r, b, t, znear, zfar);

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

/// Extract the six culling planes from a view-projection matrix, honouring the
/// active clip-space convention.
///
/// Gribb-Hartmann derives each plane from the clip-space inequality it
/// encodes, so the depth pair depends on the depth range:
///
///   [-1,1] (conventional GL):  -w <= z <= w   ->  near = r3+r2, far = r3-r2
///   [0,1]  (clip control on):   0 <= z <= w   ->  near = r2,    far = r3-r2
///
/// bromath::ffromViewProj only implements the first pair. Normalizing the
/// planes does not reconcile them — a wrong combination of rows is a wrong
/// plane at any scale — so the [0,1] case is built here.
///
/// The infinite reversed projection makes the near row constant (z_clip is
/// literally znear), which collapses that plane to "always true". A zero
/// normal would otherwise fall through to a default {0,1,0} plane and cull
/// everything below the camera, so degenerate planes are emitted as
/// unconditionally-passing instead.
inline bromath::Frustum makeFrustum(const bromath::Mat4& vp) {
    if (!gReversedZ) return bromath::ffromViewProj(vp);

    auto row = [&](int r) {
        return bromath::Vec3{vp.at(r, 0), vp.at(r, 1), vp.at(r, 2)};
    };
    auto rowW = [&](int r) { return vp.at(r, 3); };

    auto makePlane = [&](bromath::Vec3 n, float d) {
        const float L = bromath::vlen(n);
        // Degenerate == the inequality is independent of position, i.e. it
        // holds everywhere. Emit a plane every point is "inside" of.
        if (L < 1e-20f) return bromath::Plane{{0.0f, 0.0f, 0.0f}, 1e30f};
        const float inv = 1.0f / L;
        return bromath::Plane{n * inv, d * inv};
    };

    const bromath::Vec3 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    const float w0 = rowW(0), w1 = rowW(1), w2 = rowW(2), w3 = rowW(3);

    bromath::Frustum f;
    f.planes[0] = makePlane(r3 + r0, w3 + w0);   // left
    f.planes[1] = makePlane(r3 - r0, w3 - w0);   // right
    f.planes[2] = makePlane(r3 + r1, w3 + w1);   // bottom
    f.planes[3] = makePlane(r3 - r1, w3 - w1);   // top
    f.planes[4] = makePlane(r2, w2);             // near: z >= 0
    f.planes[5] = makePlane(r3 - r2, w3 - w2);   // far:  z <= w
    return f;
}

}  // namespace bro::scene
