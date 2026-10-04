#version 450

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

layout(push_constant) uniform MeshPushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 emissive;
    vec4 pbrParams; // x: metallic, y: roughness, z: alphaCutoff, w: flags
} push;

const float uWindTime = 0.0;

//__USER_CHUNK__

void main() {
    vec3 pos = inPos;
    vec3 normal = inNormal;
    vec2 uv = inUV;
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif

    vec4 worldPos = push.model * vec4(pos, 1.0);
    outWorldPos = worldPos.xyz;
    gl_Position = camera.viewProj * worldPos;

    mat3 normalMatrix = transpose(inverse(mat3(push.model)));
    vec3 N = normalize(normalMatrix * normal);
    vec3 T = normalize(normalMatrix * inTangent.xyz);
    vec3 B = cross(N, T) * inTangent.w;

    outNormal = N;
    outTangent = T;
    outBitangent = B;
    outUV = uv;
    outColor = inColor * push.baseColor;
}
