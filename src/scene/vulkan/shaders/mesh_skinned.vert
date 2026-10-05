#version 450
// GPU-skinned meshes: the bone palette (final skinning matrices,
// world(bone) * inverseBind) blends position, normal and tangent before the
// model matrix, so the TBN stays right under deformation.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"

layout(set = 3, binding = 0) uniform BonePalette {
    mat4 bones[256];
} bonePalette;

//__USER_CHUNK__

void main() {
    mat4 skin = inWeights.x * bonePalette.bones[inJoints.x] +
                inWeights.y * bonePalette.bones[inJoints.y] +
                inWeights.z * bonePalette.bones[inJoints.z] +
                inWeights.w * bonePalette.bones[inJoints.w];
    // Unweighted vertices stay in bind pose rather than collapsing to the origin.
    if (inWeights.x + inWeights.y + inWeights.z + inWeights.w < 0.001) skin = mat4(1.0);

    mat3 M3 = mat3(push.model);
    mat3 invM3 = inverse(M3);
    mat3 skinN = mat3(skin);
    vec3 pos = (skin * vec4(inPos, 1.0)).xyz;
    vec3 normal = skinN * inNormal;
    vec2 uv = inUV;
    pos += invM3 * windDelta((push.model * vec4(pos, 1.0)).xyz, inColor.r * push.extra.z);
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif

    vec4 worldPos = push.model * vec4(pos, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    vec3 N = normalize(transpose(invM3) * normal);
    vec3 T = normalize(M3 * (skinN * inTangent.xyz));
    outNormal = N;
    outTangent = T;
    outBitangent = cross(N, T) * inTangent.w;
    outUV = uv;
    outColor = meshVertexColorMode() != 0u ? inColor : vec4(1.0);
    outInstColor = vec4(1.0);
}
