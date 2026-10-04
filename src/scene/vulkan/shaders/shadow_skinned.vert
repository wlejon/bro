#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Skinned vertex attributes
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4  inWeights;

layout(set = 0, binding = 0) uniform BonePalette {
    mat4 bones[256];
} bonePalette;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightMVP;
} push;

const float uWindTime = 0.0;

//__USER_CHUNK__

void main() {
    mat4 skinMatrix = inWeights.x * bonePalette.bones[inJoints.x] +
                      inWeights.y * bonePalette.bones[inJoints.y] +
                      inWeights.z * bonePalette.bones[inJoints.z] +
                      inWeights.w * bonePalette.bones[inJoints.w];

    vec4 skinnedPos = skinMatrix * vec4(inPos, 1.0);
    vec3 pos = skinnedPos.xyz;
    mat3 skinNormMat = mat3(skinMatrix);
    vec3 normal = skinNormMat * inNormal;
    vec2 uv = inUV;
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif

    gl_Position = push.lightMVP * vec4(pos, 1.0);
}
