#version 450
// Instanced meshes: per-instance 4x3 row-major transform and RGBA record
// (locations 8-11) under the node's world matrix. The record's RGB tints the
// albedo; its alpha is the atlas cell (mesh.frag), never coverage.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
layout(location = 8)  in vec4 inInstRow0;
layout(location = 9)  in vec4 inInstRow1;
layout(location = 10) in vec4 inInstRow2;
layout(location = 11) in vec4 inInstColor;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"

//__USER_CHUNK__

void main() {
    vec3 pos = inPos;
    vec3 normal = inNormal;
    vec2 uv = inUV;
#ifdef CUSTOM_VERTEX
    // Before the instance transform, so a displacement applies to every
    // instance in its own local frame.
    userVertex(pos, normal, uv);
#endif

    mat3 R = mat3(vec3(inInstRow0.x, inInstRow1.x, inInstRow2.x),
                  vec3(inInstRow0.y, inInstRow1.y, inInstRow2.y),
                  vec3(inInstRow0.z, inInstRow1.z, inInstRow2.z));
    vec3 trans = vec3(inInstRow0.w, inInstRow1.w, inInstRow2.w);
    vec4 worldPos = push.model * vec4(R * pos + trans, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    mat3 M3 = mat3(push.model) * R;
    vec3 N = normalize(transpose(inverse(M3)) * normal);
    vec3 T = normalize(M3 * inTangent.xyz);
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T) * inTangent.w;
    outUV = uv;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
    outInstColor = inInstColor;
}
