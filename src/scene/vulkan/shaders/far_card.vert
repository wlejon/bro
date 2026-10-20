#version 450
// A far card (scene_far_cards.h): a grid of quads over its tile, one each
// FAR_CARD_GRID_TEXELS texels a side, each grid point stood at the nearest depth the tile holds round
// it (the cell's centre where it holds none) and taken back through the
// capture's view-projection to where it lay, relative to the eye now, then
// through the camera. An instance a card, its record in CARDS.

#include "far_card_layout.h"

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    mat4 viewProj;
} camera;

layout(std430, set = 1, binding = 0) readonly buffer Cards { vec4 r[]; } cards;
layout(set = 1, binding = 1) uniform usampler2D atlas;   // far_card_layout.h's texels

layout(location = 0) out vec2 outTile;        // 0..1 across the tile
layout(location = 1) flat out uint outCard;

void main() {
    uint card = uint(gl_InstanceIndex);
    uint base = card * uint(FAR_CARD_RECORD_VEC4S);
    mat4 back = mat4(cards.r[base], cards.r[base + 1u], cards.r[base + 2u], cards.r[base + 3u]);
    vec4 tile = cards.r[base + 4u];
    vec4 info = cards.r[base + 5u];
    vec4 range = cards.r[base + 6u];

    // FAR_CARD_GRID_TEXELS texels a quad (the draw's vertex count says the same).
    float texels = info.w;
    uint grid = max(1u, uint(texels) / uint(FAR_CARD_GRID_TEXELS));
    const uvec2 corner[6] = uvec2[](uvec2(0, 0), uvec2(1, 0), uvec2(0, 1), uvec2(0, 1), uvec2(1, 0), uvec2(1, 1));
    uint q = uint(gl_VertexIndex) / 6u;
    uvec2 g = uvec2(q % grid, q / grid) + corner[uint(gl_VertexIndex) % 6u];
    vec2 u = vec2(g) / float(grid);

    // The nearest depth within half a grid step round the point, on a 4x4
    // pattern of texels (as a fraction of the card's depth range).
    float size = float(textureSize(atlas, 0).x);
    vec2 origin = tile.xy * size;
    float step = texels / float(grid);
    bool any = false;
    uint nearest = REVERSED_Z ? 0u : FAR_CARD_DEPTH_MAX;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            vec2 t = u * texels + (vec2(i, j) - 1.5) * (step / 3.0);
            ivec2 at = ivec2(origin + clamp(t, vec2(0.0), vec2(texels - 1.0)));
            uint d = texelFetch(atlas, at, 0).y;
            if ((d >> FAR_CARD_DEPTH_BITS) == 0u) continue;
            d &= FAR_CARD_DEPTH_MAX;
            nearest = REVERSED_Z ? max(nearest, d) : min(nearest, d);
            any = true;
        }
    float z = any ? mix(range.x, range.y, float(nearest) / float(FAR_CARD_DEPTH_MAX)) : info.y;

    vec4 p = back * vec4(u * 2.0 - 1.0, z, 1.0);
    gl_Position = camera.viewProj * vec4(p.xyz / p.w, 1.0);
    outTile = u;
    outCard = card;
}
