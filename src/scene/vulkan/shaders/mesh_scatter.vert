#version 450
// Foliage scatter: one instance per leaf, its transform synthesised from the
// node's branch-segment records (scene_procedural.glsl) rather than read from
// an instance stream, mirroring bromesh::placeLeavesOnBranches — the same
// up bias, tilt / roll / scale jitter and leaf frame (+Z tip, +Y card normal,
// +X side). The random stream is a 32-bit hash keyed on the instance index,
// so placement is deterministic though leaf for leaf unlike the CPU's.
//
// Records: (from.xyz, radius), (dir.xyz, leaf count) per segment, then each
// leaf's segment index, four to a record from header[2].y. Header:
//   [0] seed (uint bits), up bias, tilt jitter, roll jitter
//   [1] base scale, scale jitter, scale by radius, reference radius
//   [2] density falloff, first leaf-index record

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"

// Chris Wellons' triple32-lite.
uint hashU(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}

vec3 normalizeOr(vec3 v, vec3 fallback) {
    return dot(v, v) < 1e-8 ? normalize(fallback) : normalize(v);
}

// Rodrigues rotation of v about a unit axis.
vec3 rotAxis(vec3 v, vec3 axis, float ang) {
    float c = cos(ang), s = sin(ang);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

void main() {
    vec4 h0 = procedural.header[0];
    vec4 h1 = procedural.header[1];
    vec4 h2 = procedural.header[2];
    float upBias = h0.y, tiltJitter = h0.z, rollJitter = h0.w;
    float baseScale = h1.x, scaleJitter = h1.y, scaleByRadius = h1.z, refRadius = h1.w;
    float densityFalloff = h2.x;

    int leaf = gl_InstanceIndex;
    int seg = int(procedural.records[int(h2.y) + leaf / 4][leaf % 4] + 0.5);
    vec4 t0 = procedural.records[seg * 2];
    vec4 t1 = procedural.records[seg * 2 + 1];
    vec3 from = t0.xyz;
    float radius = t0.w;
    vec3 dir = t1.xyz;

    float len = length(dir);
    if (len < 1e-6) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
    vec3 T = dir / len;

    uint base = hashU(floatBitsToUint(h0.x) ^ hashU(uint(leaf) + 1u));
    #define DRAW(k) (float(hashU(base + uint(k) * 0x9e3779b9u) >> 8) * (1.0 / 16777216.0))

    float u = DRAW(0);
    float along = densityFalloff > 0.0 ? 1.0 - pow(1.0 - u, 1.0 + densityFalloff) : u;
    vec3 P = from + dir * along;

    float phi = DRAW(1) * PROC_TWO_PI;
    vec3 e1 = perpendicularUnit(T);
    vec3 e2 = cross(T, e1);
    vec3 Rd = e1 * cos(phi) + e2 * sin(phi);

    const vec3 worldUp = vec3(0.0, 1.0, 0.0);
    vec3 F = normalizeOr(Rd * (1.0 - upBias) + worldUp * upBias, Rd);
    if (tiltJitter > 0.0) {
        float tiltAng = (DRAW(2) * 2.0 - 1.0) * tiltJitter;
        F = normalize(rotAxis(F, normalizeOr(cross(F, T), e2), tiltAng));
    }

    vec3 sideAxis = cross(F, worldUp);
    if (dot(sideAxis, sideAxis) < 1e-8) sideAxis = cross(F, vec3(1.0, 0.0, 0.0));
    sideAxis = normalize(sideAxis);
    vec3 N = normalize(cross(sideAxis, F));
    if (rollJitter > 0.0) {
        float rollAng = (DRAW(3) * 2.0 - 1.0) * rollJitter;
        sideAxis = normalize(rotAxis(sideAxis, F, rollAng));
        N = normalize(rotAxis(N, F, rollAng));
    }

    float jitter = (DRAW(4) * 2.0 - 1.0) * scaleJitter;
    float radiusFactor = 1.0;
    if (scaleByRadius > 0.0) {
        float ratio = radius > 0.0 ? radius / refRadius : 1.0;
        radiusFactor = max(1.0 + scaleByRadius * (ratio - 1.0), 0.05);
    }
    float scale = baseScale * (1.0 + jitter) * radiusFactor;
    if (scale < 1e-6) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }

    // Leaf frame columns: X side, Y normal, Z forward, each scaled.
    mat3 R = mat3(sideAxis * scale, N * scale, F * scale);
    vec4 worldPos = push.model * vec4(R * inPos + P, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    mat3 M3 = mat3(push.model) * R;
    vec3 Nw = normalize(transpose(inverse(M3)) * inNormal);
    vec3 Tw = normalize(M3 * inTangent.xyz);
    outNormal = Nw;
    outTangent = Tw;
    outBitangent = cross(Nw, Tw) * inTangent.w;
    outUV = inUV;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
    outInstColor = vec4(1.0);
}
