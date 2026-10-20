#version 450
// A far card (scene_far_cards.h): a grid of quads over its tile, one each
// FAR_CARD_GRID_TEXELS texels a side, each grid point stood at the nearest
// depth of the quads round it (the cell's centre where they hold none) and
// taken back through the capture's view-projection to where it lay,
// relative to the eye now, then through the camera. An instance a card, its
// record in CARDS.

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
layout(set = 1, binding = 2) uniform usampler2D cells;   // and its cells' nearest depths

layout(location = 0) out vec2 outTile;        // 0..1 across the tile
layout(location = 1) flat out uint outCard;

void main() {
    uint card = uint(gl_InstanceIndex);
    uint base = card * uint(FAR_CARD_RECORD_VEC4S);
    mat4 back = mat4(cards.r[base], cards.r[base + 1u], cards.r[base + 2u], cards.r[base + 3u]);
    vec4 tile = cards.r[base + 4u];
    vec4 info = cards.r[base + 5u];
    vec4 range = cards.r[base + 6u];

    // FAR_CARD_GRID_TEXELS texels a quad (the draw's index count says the
    // same): the vertex is the grid point (grid + 1 a row) the index names.
    float texels = info.w;
    uint grid = max(1u, uint(texels) / uint(FAR_CARD_GRID_TEXELS));
    uvec2 g = uvec2(uint(gl_VertexIndex) % (grid + 1u), uint(gl_VertexIndex) / (grid + 1u));
    vec2 u = vec2(g) / float(grid);

    // The nearest depth of the grid cells round the point (a cell a quad),
    // so every quad lies in front of the texels it covers; the fallback
    // where none of them holds any.
    ivec2 origin = ivec2(tile.xy * float(textureSize(atlas, 0).x) + 0.5) / FAR_CARD_GRID_TEXELS;
    uint key = 0u;
    for (int j = -1; j <= 0; ++j)
        for (int i = -1; i <= 0; ++i) {
            ivec2 c = clamp(ivec2(g) + ivec2(i, j), ivec2(0), ivec2(int(grid) - 1));
            key = max(key, texelFetch(cells, origin + c, 0).x);
        }
    float z = info.y;
    if (key != 0u) {
        uint nearest = REVERSED_Z ? key - 1u : FAR_CARD_DEPTH_MAX - (key - 1u);
        z = mix(range.x, range.y, float(nearest) / float(FAR_CARD_DEPTH_MAX));
    }

    vec4 p = back * vec4(u * 2.0 - 1.0, z, 1.0);
    gl_Position = camera.viewProj * vec4(p.xyz / p.w, 1.0);
    outTile = u;
    outCard = card;
}
