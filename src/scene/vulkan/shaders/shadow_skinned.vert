#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 5) in uvec4 inJoints;
layout(location = 6) in vec4 inWeights;

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"

layout(set = 3, binding = 0) uniform BonePalette {
    mat4 bones[256];
} bonePalette;

//__USER_CHUNK__

void main() {
    mat4 skin = inWeights.x * bonePalette.bones[inJoints.x] +
                inWeights.y * bonePalette.bones[inJoints.y] +
                inWeights.z * bonePalette.bones[inJoints.z] +
                inWeights.w * bonePalette.bones[inJoints.w];
    if (inWeights.x + inWeights.y + inWeights.z + inWeights.w < 0.001) skin = mat4(1.0);

    mat4 model = casterModel();
    vec3 pos = (skin * vec4(inPos, 1.0)).xyz;
    vec3 normal = mat3(skin) * inNormal;
    vec2 uv = inUV;
    pos += inverse(mat3(model)) * windDelta((model * vec4(pos, 1.0)).xyz, inColor.r * push.params.x);
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif
    gl_Position = push.lightViewProj * (model * vec4(pos, 1.0));
}
