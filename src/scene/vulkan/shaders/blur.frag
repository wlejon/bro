#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform BlurPushConstants {
    vec2 uDir;
    vec2 padding;
} push;

const float w0 = 0.2270270270;
const float w1 = 0.1945945946;
const float w2 = 0.1216216216;
const float w3 = 0.0540540541;
const float w4 = 0.0162162162;

void main() {
    vec4 c = texture(uTex, inUV) * w0;
    c += texture(uTex, inUV + push.uDir * 1.0) * w1;
    c += texture(uTex, inUV - push.uDir * 1.0) * w1;
    c += texture(uTex, inUV + push.uDir * 2.0) * w2;
    c += texture(uTex, inUV - push.uDir * 2.0) * w2;
    c += texture(uTex, inUV + push.uDir * 3.0) * w3;
    c += texture(uTex, inUV - push.uDir * 3.0) * w3;
    c += texture(uTex, inUV + push.uDir * 4.0) * w4;
    c += texture(uTex, inUV - push.uDir * 4.0) * w4;
    outColor = c;
}
