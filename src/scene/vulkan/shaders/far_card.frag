#version 450
// A far card's fragment (scene_far_cards.h): its texel's colour, on the
// share of the samples its crowns covered of the pixel's
// samples (dithered as mesh.frag's forestSampleMask keeps a cover), at the
// depth its nearest texel holds, reprojected into the camera. Built twice:
// as is, and with FAR_CARD_INDIRECT for the SSAO opaque scope (no indirect
// light of its own to darken).

#include "far_card_layout.h"

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;
// The target is 4x multisampled (each sample its own share of the cover).
layout(constant_id = 1) const bool MSAA = true;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
} camera;

layout(std430, set = 1, binding = 0) readonly buffer Cards { vec4 r[]; } cards;
layout(set = 1, binding = 1) uniform usampler2D atlas;   // far_card_layout.h's texels

layout(location = 0) in vec2 inTile;
layout(location = 1) flat in uint inCard;

layout(location = 0) out vec4 outColor;
#ifdef FAR_CARD_INDIRECT
layout(location = 1) out vec4 outIndirect;
#endif
// Never nearer than the card's grid, which lies in front of its texels: the
// depth test can run before the fragment (REVERSED_Z; as mesh.frag, the
// other policy keeps the grid's depth).
layout(depth_less) out float gl_FragDepth;

vec3 unpackRGB9E5(uint v) {
    float scale = exp2(float(int(v >> 27) - 24));
    return vec3(uvec3(v, v >> 9, v >> 18) & uvec3(511u)) * scale;
}

void main() {
    uint base = inCard * uint(FAR_CARD_RECORD_VEC4S);
    vec4 tile = cards.r[base + 4u];
    vec4 info = cards.r[base + 5u];
    vec4 range = cards.r[base + 6u];
    float size = float(textureSize(atlas, 0).x);
    // (Kept half a texel inside the tile: its neighbours are other cards.)
    vec2 lo = tile.xy + 0.5 / size, hi = tile.zw - 0.5 / size;
    vec2 uv = clamp(mix(tile.xy, tile.zw, inTile), lo, hi);
    // The nearest texel. A card stands in from an eye up to a pixel and a
    // half off, so its texels fall between the screen's pixels; any filter
    // across them blurs the crowns' pixel-sized leaves (bilinear and
    // Catmull-Rom both read soft beside the live crowns), while the step
    // nearest takes as the eye moves is lost in the crowns' own
    // frame-to-frame shimmer.
    uvec2 texel = texelFetch(atlas, ivec2(uv * size), 0).xy;
    uint covered = texel.y >> FAR_CARD_DEPTH_BITS;
    if (covered == 0u) discard;
    float cover = float(covered) / range.z;
    float z = mix(range.x, range.y, float(texel.y & FAR_CARD_DEPTH_MAX) / float(FAR_CARD_DEPTH_MAX));
    mat4 back = mat4(cards.r[base], cards.r[base + 1u], cards.r[base + 2u], cards.r[base + 3u]);
    vec4 p = back * vec4(inTile * 2.0 - 1.0, z, 1.0);
    vec4 clip = camera.viewProj * vec4(p.xyz / p.w, 1.0);
    gl_FragDepth = REVERSED_Z ? min(clamp(clip.z / clip.w, 0.0, 1.0), gl_FragCoord.z) : gl_FragCoord.z;

    // The samples it keeps: `cover` of them, in its window [info.x, 1) of
    // the pixel's dither (the real crowns' the rest).
    uvec2 q = uvec2(gl_FragCoord.xy);
    uint v = q.x * 1973u + q.y * 9277u + 0x9e3779b9u;
    v = (v ^ (v >> 16u)) * 0x7feb352du;
    v = (v ^ (v >> 15u)) * 0x846ca68bu;
    v ^= v >> 16u;
    float h = float(v & 0xffffu) * (1.0 / 65536.0);
    uint v2 = v ^ (floatBitsToUint(info.z) * 0x9e3779b9u);
    v2 = (v2 ^ (v2 >> 15u)) * 0x2c1b3c6du;
    v2 ^= v2 >> 12u;
    float h2 = float(v2 >> 16u) * (1.0 / 65536.0);
    int mask = 0;
    if (MSAA) {
        for (int s = 0; s < 4; ++s) {
            float hs = fract(h + 0.25 * float(s));
            float cs = fract(h2 + 0.25 * float(s));
            if (hs >= info.x && cs < cover) mask |= 1 << s;
        }
    } else {
        mask = h >= info.x && h2 < cover ? -1 : 0;
    }
    if (mask == 0) discard;
    gl_SampleMask[0] = mask;

    outColor = vec4(unpackRGB9E5(texel.x), 1.0);
#ifdef FAR_CARD_INDIRECT
    outIndirect = vec4(0.0);
#endif
}
