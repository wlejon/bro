// GPU foliage-scatter vertex shader. Instead of reading a per-instance model
// transform from an instance VBO (mesh_instanced.vert), this shader SYNTHESISES
// each leaf's transform on the GPU from a compact per-segment buffer + the
// instance id. That moves the whole leaf scatter (tens of thousands of leaves)
// off the CPU: the app uploads ~a few hundred segments, and the draw expands
// them into leaves entirely in the vertex shader — no 16-floats-per-leaf buffer
// is ever built or uploaded.
//
// It mirrors bromesh::placeLeavesOnBranches (leaf_scatter.cpp) so the look
// matches the CPU path — same up-bias / tilt / roll / scale jitter, same leaf
// local frame (+Z tip, +Y card normal, +X side). The RNG is a 32-bit hash
// stream rather than the CPU's splitmix64, so individual leaf positions differ,
// but the statistical distribution (hence the look) is identical.
//
// Two buffers, both re-uploaded only when the sim grows:
//   uSegments (samplerBuffer RGBA32F), 2 texels per segment:
//     texel 2*i     = (from.xyz, radius)
//     texel 2*i + 1 = (dir.xyz,  unused)     dir = to - from (not normalised)
//   uInstSeg  (samplerBuffer R32F), one texel per leaf = its segment index.
// The draw issues exactly one instance per leaf (no padding / degenerate
// slots): the CPU expands each segment's precomputed leaf count into that many
// entries of uInstSeg, so gl_InstanceID maps 1:1 to a real leaf. Each leaf's
// random stream is hashed from gl_InstanceID, so placement is deterministic.

#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aTangent;

uniform mat4 uVP;          // projection * view (no translation; camera-relative)
uniform vec3 uCameraEye;   // world-space eye, subtracted from the leaf origin
uniform mat4 uInstModel;   // node's parent-chain world transform
uniform int  uUseVertexColor;

uniform samplerBuffer uSegments;   // packed per-segment records (2 texels each)
uniform samplerBuffer uInstSeg;    // per-leaf segment index (R32F)
uniform uint  uScatterSeed;        // deterministic seed
uniform float uUpBias;
uniform float uTiltJitter;
uniform float uRollJitter;
uniform float uBaseScale;
uniform float uScaleJitter;
uniform float uScaleByRadius;
uniform float uRefRadius;          // = max leaf-placement radius (scaleByRadius ref)
uniform float uDensityFalloff;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vColor;
out float vCamDist;
out vec3 vTangentW;
out vec3 vBitangentW;
out vec4 vInstColor;

const float TWO_PI = 6.28318530718;

// 32-bit integer hash (Chris Wellons' triple32-lite). Good avalanche; ample for
// leaf jitter. Draws below advance a per-leaf counter through it.
uint hashU(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

vec3 normalizeOr(vec3 v, vec3 fallback) {
    return (dot(v, v) < 1e-8) ? normalize(fallback) : normalize(v);
}

// Any unit vector perpendicular to unit `t` — matches perpendicularUnit().
vec3 perpendicularUnit(vec3 t) {
    vec3 c = cross(t, vec3(0.0, 1.0, 0.0));
    if (dot(c, c) < 1e-8) c = cross(t, vec3(1.0, 0.0, 0.0));
    return normalize(c);
}

// Rodrigues rotation of v about unit axis by ang — matches qrotate(qaxisAngle).
vec3 rotAxis(vec3 v, vec3 axis, float ang) {
    float c = cos(ang), s = sin(ang);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

void main() {
    // One instance per leaf: look up which segment this leaf belongs to.
    int seg = int(texelFetch(uInstSeg, gl_InstanceID).r + 0.5);

    vec4 t0 = texelFetch(uSegments, seg * 2);
    vec4 t1 = texelFetch(uSegments, seg * 2 + 1);
    vec3 from   = t0.xyz;
    float radius = t0.w;
    vec3 dir    = t1.xyz;

    float len = length(dir);
    if (len < 1e-6) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    vec3 T = dir / len;

    // Per-leaf hash stream keyed on the global instance id. draw(k) returns a
    // fresh 24-bit float in [0,1).
    uint base = hashU(uScatterSeed ^ hashU(uint(gl_InstanceID) + 1u));
    #define DRAW(k) (float(hashU(base + uint(k) * 0x9e3779b9u) >> 8) * (1.0 / 16777216.0))

    float u = DRAW(0);
    float tt = (uDensityFalloff > 0.0)
             ? 1.0 - pow(1.0 - u, 1.0 + uDensityFalloff)
             : u;
    vec3 P = from + dir * tt;

    float phi = DRAW(1) * TWO_PI;
    vec3 e1 = perpendicularUnit(T);
    vec3 e2 = cross(T, e1);
    vec3 Rd = e1 * cos(phi) + e2 * sin(phi);

    vec3 worldUp = vec3(0.0, 1.0, 0.0);
    vec3 Fraw = Rd * (1.0 - uUpBias) + worldUp * uUpBias;
    vec3 F = normalizeOr(Fraw, Rd);

    if (uTiltJitter > 0.0) {
        float tiltAng = (DRAW(2) * 2.0 - 1.0) * uTiltJitter;
        vec3 tiltAxis = normalizeOr(cross(F, T), e2);
        F = normalize(rotAxis(F, tiltAxis, tiltAng));
    }

    vec3 sideAxis = cross(F, worldUp);
    if (dot(sideAxis, sideAxis) < 1e-8) sideAxis = cross(F, vec3(1.0, 0.0, 0.0));
    sideAxis = normalize(sideAxis);
    vec3 N = normalize(cross(sideAxis, F));

    if (uRollJitter > 0.0) {
        float rollAng = (DRAW(3) * 2.0 - 1.0) * uRollJitter;
        sideAxis = normalize(rotAxis(sideAxis, F, rollAng));
        N        = normalize(rotAxis(N, F, rollAng));
    }

    float jitter = (DRAW(4) * 2.0 - 1.0) * uScaleJitter;
    float radiusFactor = 1.0;
    if (uScaleByRadius > 0.0) {
        float ratio = (radius > 0.0) ? (radius / uRefRadius) : 1.0;
        radiusFactor = 1.0 + uScaleByRadius * (ratio - 1.0);
        if (radiusFactor < 0.05) radiusFactor = 0.05;
    }
    float scale = uBaseScale * (1.0 + jitter) * radiusFactor;
    if (scale < 1e-6) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }

    // Leaf local frame → world. Columns: X = side*scale, Y = normal*scale,
    // Z = forward*scale; translation = P. Matches writeMatrix() in leaf_scatter.
    mat3 R = mat3(sideAxis * scale, N * scale, F * scale);
    vec3 lPos    = aPos;
    vec3 lNormal = aNormal;

    vec3 worldPos = (uInstModel * vec4(R * lPos + P, 1.0)).xyz;
    vec3 camRel = worldPos - uCameraEye;
    mat3 normalMat = mat3(uInstModel) * R;

    vWorldPos = camRel;
    vNormal = normalMat * lNormal;
    vTangentW   = normalMat * aTangent.xyz;
    vBitangentW = cross(vNormal, vTangentW) * aTangent.w;
    vUV = aUV;
    vColor = (uUseVertexColor == 1) ? aColor : vec4(1.0);
    vCamDist = length(camRel);
    vInstColor = vec4(1.0);
    gl_Position = uVP * vec4(camRel, 1.0);
}
