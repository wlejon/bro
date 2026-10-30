#version 450
// The volumes' reconstruction (PassVolume): this render's volumes (light,
// transmittance) blended with last render's result where the pixel's medium
// was then. The medium's depth this render places the pixel's light in
// space (no medium: as far as the sky, its direction alone); last render's
// camera finds it there; what is found is clamped to the range of the
// pixel's 3x3 neighbourhood this render, so a medium that moved or opened
// is not smeared from where it was, and takes the rest of the blend.

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uMedium;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
layout(set = 0, binding = 2) uniform sampler2D uHistory;
layout(set = 0, binding = 3) uniform ResolveUBO {
    mat4 prevViewProj;   // last render's camera-relative clip
    mat4 invViewProj;    // this render's, inverted
    vec4 eyeDelta;       // xyz: this render's eye - last render's; w: this render's share
} ubo;

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(uMedium, 0) - 1;
    vec4 c = texelFetch(uMedium, px, 0);
    float share = ubo.eyeDelta.w;
    if (share >= 1.0) {
        outColor = c;
        return;
    }
    // The neighbourhood's range, and its farthest medium: a pixel just off
    // a cloud's edge is reprojected with the cloud beside it, so the edge
    // is found where it was rather than the sky behind it.
    vec4 lo = c, hi = c;
    float depth = texelFetch(uDepth, px, 0).r;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), last);
            vec4 s = texelFetch(uMedium, q, 0);
            lo = min(lo, s);
            hi = max(hi, s);
            if (depth <= 0.0) depth = texelFetch(uDepth, q, 0).r;
        }
    }
    vec4 p = ubo.invViewProj * vec4(inUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(p.xyz / p.w);
    vec4 clip = depth > 0.0 ? ubo.prevViewProj * vec4(dir * depth + ubo.eyeDelta.xyz, 1.0)
                            : ubo.prevViewProj * vec4(dir, 0.0);
    if (clip.w <= 0.0) {
        outColor = c;
        return;
    }
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
        outColor = c;
        return;
    }
    vec4 h = clamp(texture(uHistory, uv), lo, hi);
    outColor = mix(h, c, share);
}
