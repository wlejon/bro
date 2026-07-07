// GGX prefilter: builds the specular IBL mip chain. Mip k holds the env
// convolved with a GGX lobe at roughness = k / (mipCount - 1). At runtime
// the PBR shader does `prefilter(R, roughness * lastMip)` and combines
// with the BRDF LUT (the split-sum approximation of Karis 2013).
//
// Per-fragment: importance-sample the GGX distribution with a Hammersley
// sequence, accumulate weighted env samples along the reflected directions.
// The `uEnvSize` uniform feeds Krivanek's mip-bias trick so very few-sample
// fragments don't firefly from sparse high-frequency taps.

#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform samplerCube uEnv;
uniform int   uFace;
uniform float uRoughness;
uniform float uEnvSize;     // resolution of mip 0 of uEnv (e.g. 512)

const float PI = 3.14159265358979;

vec3 cubeDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));
    if (face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));
    if (face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));
    if (face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));
    if (face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));
    return            normalize(vec3(-uv.x, -uv.y, -1.0));
}

// Van der Corput sequence (radical-inverse base 2).
float radicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}
vec2 hammersley(uint i, uint N) {
    return vec2(float(i) / float(N), radicalInverse_VdC(i));
}

vec3 importanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    vec3 up        = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent   = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    vec3 N = cubeDir(uFace, uv);
    // Split-sum approximation: V = R = N. Mostly correct for diffuse-ish
    // angles; the BRDF LUT corrects the rest at runtime.
    vec3 R = N;
    vec3 V = R;

    const uint SAMPLE_COUNT = 1024u;
    float totalWeight = 0.0;
    vec3  prefiltered = vec3(0.0);
    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec2 Xi = hammersley(i, SAMPLE_COUNT);
        vec3 H  = importanceSampleGGX(Xi, N, uRoughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            // Krivanek mip bias: sample from a higher mip when the GGX pdf
            // is low for this tap, eliminating bright firefly samples.
            float D     = distributionGGX(N, H, uRoughness);
            float NdotH = max(dot(N, H), 0.0);
            // Floor HdotV inside the reciprocal — otherwise HdotV→0 drives
            // pdf→Inf and the log2 below to -Inf, producing NaN/Inf LOD.
            float HdotV = max(dot(H, V), 1e-4);
            float pdf   = D * NdotH / (4.0 * HdotV) + 1e-4;

            float saTexel  = 4.0 * PI / (6.0 * uEnvSize * uEnvSize);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf);
            // Clamp log2 argument away from zero, and clamp the final mip to
            // a sane range so textureLod never sees -Inf or a mip beyond the
            // source cubemap's last level.
            float mipLevel = uRoughness == 0.0 ? 0.0
                           : clamp(0.5 * log2(max(saSample / saTexel, 1e-8)),
                                   0.0, 16.0);

            // Guard the env tap itself: some HDRs carry tiny negative pixels
            // from upstream tonemapping, and stray Inf values can sneak past
            // importance sampling at very low roughness. Both poison the
            // accumulation if left unchecked.
            vec3 s = textureLod(uEnv, L, mipLevel).rgb;
            s = max(s, vec3(0.0));
            if (!any(isnan(s)) && !any(isinf(s))) {
                prefiltered += s * NdotL;
                totalWeight += NdotL;
            }
        }
    }
    // totalWeight can legitimately be 0 at extreme roughnesses where every
    // importance-sampled direction missed; fall back to the coarsest mip of
    // the source at N (a rough approximation, but keeps the output finite).
    if (totalWeight > 0.0) {
        prefiltered /= totalWeight;
    } else {
        prefiltered = max(textureLod(uEnv, N, 16.0).rgb, vec3(0.0));
    }
    FragColor = vec4(prefiltered, 1.0);
}
