#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

// Skinned vertex attributes
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4  inWeights;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
    mat4 invView;
    mat4 invProj;
    vec4 eyePos;
    vec4 viewport;
    vec4 fogParams;
    vec4 fogColor;
} camera;

layout(set = 3, binding = 0) uniform BonePalette {
    mat4 bones[256];
} bonePalette;

layout(push_constant) uniform MeshPushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 emissive;
    vec4 pbrParams; // x: metallic, y: roughness, z: alphaCutoff, w: flags
} push;

void main() {
    mat4 skinMatrix = inWeights.x * bonePalette.bones[inJoints.x] +
                      inWeights.y * bonePalette.bones[inJoints.y] +
                      inWeights.z * bonePalette.bones[inJoints.z] +
                      inWeights.w * bonePalette.bones[inJoints.w];

    vec4 skinnedPos = skinMatrix * vec4(inPos, 1.0);
    vec4 worldPos = push.model * skinnedPos;
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    mat3 normalMatrix = transpose(inverse(mat3(push.model) * mat3(skinMatrix)));
    vec3 N = normalize(normalMatrix * inNormal);
    vec3 T = normalize(normalMatrix * inTangent.xyz);
    vec3 B = cross(N, T) * inTangent.w;

    outNormal = N;
    outTangent = T;
    outBitangent = B;
    outUV = inUV;
    outColor = inColor * push.baseColor;
}
