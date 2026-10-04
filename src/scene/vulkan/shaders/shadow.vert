#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightMVP;
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
    gl_Position = push.lightMVP * vec4(pos, 1.0);
}
