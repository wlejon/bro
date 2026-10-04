#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texColor;
layout(set = 0, binding = 1) uniform sampler3D texLut;

layout(push_constant) uniform ColorLutPushConstants {
    float amount;
    float scale;
    float offset;
    float padding;
} push;

void main() {
    vec4 src = texture(texColor, inUV);
    if (push.amount <= 0.0) {
        outColor = src;
        return;
    }
    vec3 graded = texture(texLut, src.rgb * push.scale + push.offset).rgb;
    outColor = vec4(mix(src.rgb, graded, push.amount), src.a);
}
