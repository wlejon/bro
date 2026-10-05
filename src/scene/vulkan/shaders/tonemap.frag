#version 450
// HDR to LDR: the blurred bloom added in HDR, exposure, the operator (linear
// clamp, Reinhard, ACES), gamma, then the 3D colour-grading LUT, which is
// authored in display space. Each optional step is skipped when off, so the
// frame stays bit-exact without it.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D texHdr;
layout(set = 0, binding = 1) uniform sampler2D texBloom;
layout(set = 0, binding = 2) uniform sampler3D texLut;

layout(push_constant) uniform TonemapPush {
    float exposure;
    float gamma;            // applied when > 0 and not 1
    float bloomIntensity;   // 0 = bloom off
    int tonemapMode;        // 0 = linear, 1 = Reinhard, 2 = ACES
    float lutAmount;        // 0 = LUT off, 1 = fully graded
    float lutScale;         // (size - 1) / size: [0, 1] onto texel centres
    float lutOffset;        // 0.5 / size
    float pad;
} push;

// ACES approximation by Krzysztof Narkowicz.
vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 src = texture(texHdr, inUV);
    vec3 c = src.rgb;
    if (push.bloomIntensity > 0.0) c += texture(texBloom, inUV).rgb * push.bloomIntensity;
    c *= push.exposure;
    if (push.tonemapMode == 2)      c = aces(c);
    else if (push.tonemapMode == 1) c = c / (c + vec3(1.0));
    else                            c = clamp(c, 0.0, 1.0);
    if (push.gamma > 0.0 && push.gamma != 1.0) c = pow(c, vec3(1.0 / push.gamma));
    if (push.lutAmount > 0.0) {
        vec3 graded = texture(texLut, c * push.lutScale + push.lutOffset).rgb;
        c = mix(c, graded, push.lutAmount);
    }
    outColor = vec4(c, src.a);
}
