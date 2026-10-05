#version 450
// The split-sum BRDF LUT (Karis 2013): (NdotV, roughness) -> the scale and
// bias on F0 of the specular environment term. Environment-independent;
// baked once.

#include "ibl_common.glsl"

layout(location = 0) out vec2 outColor;

// Schlick-GGX with the image-based lighting k = a^2 / 2.
float geometryIBL(float NdotX, float roughness) {
    float k = roughness * roughness / 2.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}

void main() {
    float NdotV = max(inUV.x, 1e-4);
    float roughness = max(inUV.y, 1e-4);
    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);

    float A = 0.0;
    float B = 0.0;
    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec3 H = importanceSampleGGX(hammersley(i, SAMPLE_COUNT), N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0);
        float NdotH = max(H.z, 0.0);
        float VdotH = max(dot(V, H), 0.0);
        if (NdotL > 0.0) {
            float G = geometryIBL(NdotV, roughness) * geometryIBL(NdotL, roughness);
            float Gvis = G * VdotH / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * Gvis;
            B += Fc * Gvis;
        }
    }
    outColor = vec2(A, B) / float(SAMPLE_COUNT);
}
