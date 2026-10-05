// What the environment bake shaders share (scene/vulkan/scene_environment.cpp):
// the full-screen uv from postfx.vert (row 0 at v = 0), the push block, and
// the direction of a cube face texel.

layout(location = 0) in vec2 inUV;

layout(push_constant) uniform BakePush {
    int face;          // 0..5 = +X -X +Y -Y +Z -Z
    float roughness;   // prefilter: this mip's roughness
    float envSize;     // prefilter: the source cube's mip 0 size
} bake;

const float PI = 3.14159265358979;
const float TWO_PI = 6.28318530717958;

// Cube face texel (s, t in [-1, 1], t = -1 on row 0) -> direction, the
// standard cube-map convention every sampler uses.
vec3 cubeDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));
    if (face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));
    if (face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));
    if (face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));
    if (face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));
    return normalize(vec3(-uv.x, -uv.y, -1.0));
}

// Hammersley point i of n (Van der Corput radical inverse, base 2).
vec2 hammersley(uint i, uint n) {
    uint bits = i;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

// A GGX-distributed half vector around N.
vec3 importanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);
    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

// A finite, non-negative radiance sample: some HDRs carry tiny negative
// pixels or non-finite values, and either poisons every convolution after.
vec3 sanitize(vec3 s) {
    s = max(s, vec3(0.0));
    return any(isnan(s)) || any(isinf(s)) ? vec3(0.0) : s;
}
