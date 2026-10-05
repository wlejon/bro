#version 450
// Static meshes. Skinned (mesh_skinned.vert) and instanced
// (mesh_instanced.vert) meshes emit the same varyings for mesh.frag.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;   // xyz tangent, w handedness

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"

// Custom-shader splice point: a user chunk defining
// `void userVertex(inout vec3 pos, inout vec3 normal, inout vec2 uv)` replaces
// this marker (scene_vk_custom_shader.cpp) and defines CUSTOM_VERTEX.
//__USER_CHUNK__

void main() {
    mat3 M3 = mat3(push.model);
    mat3 invM3 = inverse(M3);
    vec3 pos = inPos;
    vec3 normal = inNormal;
    vec2 uv = inUV;
    // Wind sway: bent by vertex colour R times the mesh's wind mask, applied
    // in world space and carried back into object space so the hook sees the
    // swayed position.
    pos += invM3 * windDelta((push.model * vec4(pos, 1.0)).xyz, inColor.r * push.extra.z);
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif

    vec4 worldPos = push.model * vec4(pos, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    mat3 normalMatrix = transpose(invM3);
    vec3 N = normalize(normalMatrix * normal);
    vec3 T = normalize(M3 * inTangent.xyz);
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T) * inTangent.w;
    outUV = uv;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
    outInstColor = vec4(1.0);
}
