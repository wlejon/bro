#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texHdr;
layout(set = 0, binding = 1) uniform sampler2D texBloom;

layout(push_constant) uniform PostFxPushConstants {
    float exposure;
    float gamma;
    float bloomIntensity;
    int   tonemapMode; // 0 = Linear, 1 = Reinhard, 2 = ACES
    int   enableFxaa;
    float texelSizeX;
    float texelSizeY;
    float padding;
} push;

vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 hdrColor = texture(texHdr, inUV);
    vec3 col = hdrColor.rgb;

    // Add bloom if intensity > 0
    if (push.bloomIntensity > 0.0) {
        vec3 bloom = texture(texBloom, inUV).rgb;
        col += bloom * push.bloomIntensity;
    }

    // Exposure adjustment
    col *= push.exposure;

    // Tonemap operator
    if (push.tonemapMode == 1) {
        col = col / (col + vec3(1.0));
    } else if (push.tonemapMode == 2) {
        col = aces(col);
    } else {
        col = clamp(col, 0.0, 1.0);
    }

    // Gamma correction
    float g = push.gamma > 0.0 ? push.gamma : 2.2;
    col = pow(col, vec3(1.0 / g));

    outColor = vec4(col, hdrColor.a);
}
