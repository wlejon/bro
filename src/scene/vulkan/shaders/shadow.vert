#version 450

layout(location = 0) in vec3 inPos;

layout(push_constant) uniform ShadowPushConstants {
    mat4 lightMVP;
} push;

void main() {
    gl_Position = push.lightMVP * vec4(inPos, 1.0);
}
