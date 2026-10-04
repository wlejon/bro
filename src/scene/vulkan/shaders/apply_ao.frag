#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uAoTex;

layout(push_constant) uniform PushConsts {
    float uIntensity;
} push;

void main() {
    float ao = texture(uAoTex, inUV).r;
    float factor = mix(1.0, ao, push.uIntensity);
    outColor = vec4(factor, factor, factor, 1.0);
}
