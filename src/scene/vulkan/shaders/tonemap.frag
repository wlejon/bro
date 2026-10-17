#version 450
// HDR to LDR: the blurred bloom added in HDR, exposure, the operator (linear
// clamp, Reinhard, ACES), gamma, then the 3D colour-grading LUT, which is
// authored in display space, then a one-level dither for the 8-bit target
// (push.dither). Each optional step is skipped when off, so the frame stays
// bit-exact without it.

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
    float dither;           // 1 = dither the 8-bit result (SceneRenderer::setDither)
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

// Triangular-PDF noise in (-1, 1): two independent uniform hashes summed.
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
// Each channel its own, so the channels' steps (which fall on different rows
// and tint the bands) break up too.
vec3 ditherTpdf(vec2 p) {
    return vec3(hash12(p) + hash12(p + vec2(57.31, 113.7)),
                hash12(p + vec2(91.7, 23.9)) + hash12(p + vec2(11.3, 71.1)),
                hash12(p + vec2(37.1, 147.3)) + hash12(p + vec2(163.9, 5.7))) - 1.0;
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
    // The target is 8 bits a channel: a slow gradient (the sky, haze over far
    // hills, a cloud's edge) quantises into plateaus one level apart, which
    // read as contoured bands, and worse once the frame is scaled or
    // re-encoded. A triangular dither of one level, fixed per pixel so a
    // frame stays deterministic, turns each step into grain finer than the
    // eye resolves. Pure black and white stay exact (the dither fades out
    // within a level of either end).
    c = clamp(c, 0.0, 1.0);
    if (push.dither > 0.0) c += ditherTpdf(gl_FragCoord.xy) * min(vec3(1.0), min(c, 1.0 - c) * 255.0) / 255.0;
    outColor = vec4(c, src.a);
}
