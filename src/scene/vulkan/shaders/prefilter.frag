#version 450
// GGX prefilter: mip k of the output holds the source convolved with a GGX
// lobe of roughness k / (mips - 1), importance-sampled with a Hammersley
// sequence (split sum, V = R = N). Krivanek's mip bias reads a blurrier mip of
// the source where a tap's pdf is low, so sparse taps do not firefly.

#include "ibl_common.glsl"

layout(set = 0, binding = 0) uniform samplerCube texEnv;
layout(location = 0) out vec4 outColor;

float distributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

void main() {
    vec3 N = cubeDir(bake.face, inUV * 2.0 - 1.0);
    vec3 V = N;
    float roughness = bake.roughness;

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3 prefiltered = vec3(0.0);
    float saTexel = 4.0 * PI / (6.0 * bake.envSize * bake.envSize);
    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec3 H = importanceSampleGGX(hammersley(i, SAMPLE_COUNT), N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL <= 0.0) continue;
        float NdotH = max(dot(N, H), 0.0);
        float HdotV = max(dot(H, V), 1e-4);
        float pdf = distributionGGX(NdotH, roughness) * NdotH / (4.0 * HdotV) + 1e-4;
        float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf);
        float mipLevel = roughness == 0.0 ? 0.0 : clamp(0.5 * log2(max(saSample / saTexel, 1e-8)), 0.0, 16.0);
        prefiltered += sanitize(textureLod(texEnv, L, mipLevel).rgb) * NdotL;
        totalWeight += NdotL;
    }
    // No tap landed (extreme roughness): the coarsest source mip along N.
    prefiltered = totalWeight > 0.0 ? prefiltered / totalWeight : max(textureLod(texEnv, N, 16.0).rgb, vec3(0.0));
    outColor = vec4(prefiltered, 1.0);
}
