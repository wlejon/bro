#version 450

layout(location = 0) in vec3 inPos;

// Skinned vertex attributes
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4  inWeights;

layout(set = 0, binding = 0) uniform BonePalette {
    mat4 bones[256];
} bonePalette;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightMVP;
} push;

void main() {
    mat4 skinMatrix = inWeights.x * bonePalette.bones[inJoints.x] +
                      inWeights.y * bonePalette.bones[inJoints.y] +
                      inWeights.z * bonePalette.bones[inJoints.z] +
                      inWeights.w * bonePalette.bones[inJoints.w];

    vec4 skinnedPos = skinMatrix * vec4(inPos, 1.0);
    gl_Position = push.lightMVP * skinnedPos;
}
