#version 450
// Forest sprite shadow casters: mesh_forest_sprite.vert's spheres as discs
// into an atlas tile, facing the light (offset in the tile's clip space),
// as much of the light as their leaves stop (shadow_forest.frag).

layout(location = 0) out vec2 outUV;
layout(location = 1) out float outSeed;
layout(location = 2) flat out float outDepth;   // the leaves' optical depth through the middle
layout(location = 3) flat out float outShare;   // the tier's share of the tree's caster (the drawer's fade code)
layout(location = 4) flat out float outHollow;  // a broad lobe's hollow (forestShellPath), 0: leaves throughout

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#include "scene_forest_sprite_vertex.glsl"
#include "scene_forest_mass.glsl"

void main() {
    ForestSprite s;
    // The draw's mode rides in the wind-mask slot (MeshDraw push.extra.z).
    if (!forestSprite(int(push.params.x + 0.5), gl_InstanceIndex, gl_VertexIndex, casterModel(), s)) {
        gl_Position = FOREST_CULLED;
        outUV = vec2(0.0);
        outSeed = 0.0;
        outDepth = 0.0;
        outShare = 0.0;
        outHollow = 0.0;
        return;
    }
    // A broad clump's leaves lie in its lobe's shell (scene_forest_shoot.glsl's
    // shoots at 1 - shell * (0.2 .. 0.8) of its radius), a crown's in its
    // clumps.
    outHollow = int(push.params.x + 0.5) == 0 && forestLeafForm() == 0
              ? 1.0 - 0.8 * clamp(FOREST_HEADER(FOREST_H_SHELL), 0.0, 1.0) : 0.0;
    // A caster's fade code is [0, share): the light a tree's sprites stop
    // is that share of what they would, the rest its other tier's (the
    // drawer hands the tiers over continuously).
    float k = floor(s.fade * 0.5);
    outShare = clamp(s.fade - 2.0 * k - k * (1.0 / 4096.0), 0.0, 1.0);
    float r = max(s.radius, 1e-6);
    outDepth = forestMassDepth(sqrt(6.2831853 / s.leaves), s.leafLen / r, FOREST_HEADER(FOREST_H_LEAF_ASPECT));
    mat4 L = push.lightViewProj;
    // The disc stands half a radius past the centre, away from the light
    // (the tile's depth grows that way): the light reaching a leaf in the
    // sphere has crossed only the leaves above it, so the disc shades the
    // sphere's far part and what lies beyond, not its lit side. (A sprite
    // takes its own leaves' shade analytically, mesh.frag's leafSphere.)
    // A broad lobe's leaves lie in its shell, so all but its lit cap lie
    // behind the near shell's leaves: its disc stands at the centre. (Half a
    // radius on, the shell's sides and the inner face of its far half went
    // unshaded: a canopy's crowns 4 units brighter than their leaves'.)
    vec3 away = normalize(vec3(L[0][2], L[1][2], L[2][2]));
    // The leaves lean up (the emit's upBias), so they stop less of a low sun
    // than of a high one: the area they turn to the light (Ross's G, 0.5 for
    // leaves facing every way) is about (1 - b) / 2 + b |L.y|, over the 0.5
    // forestMassDepth takes. (As one depth for every sun, the far tiers'
    // casters shaded a golden canopy 8 to 12 units under its leaves'.)
    float b = clamp(1.4 * FOREST_HEADER(FOREST_H_UP_BIAS), 0.0, 1.0);
    outDepth *= (1.0 - b) + 2.0 * b * abs(away.y);
    // A crown's leaves lie in its clumps, with gaps between them, not spread
    // evenly through its sphere: the light through it meets them unevenly,
    // and the mean that passes is more than through as many leaves spread
    // evenly (forestry's clumping index, about half for a crown of clumps).
    // As an even disc, a low sun's long shadows across a far canopy shaded
    // its crowns 7 units under its leaves'.
    if (int(push.params.x + 0.5) == 1) outDepth *= FOREST_CROWN_CLUMPING;
    // A spray node's crown (a needle or scale-leaf form) is its sprays' pads
    // filled to their rims, dense through: its disc stands a quarter radius
    // on, shading more of its near half. (Half a radius on, a cypress's
    // crowns were 8 to 13 units brighter than its leaves at 45 / 75 m.)
    bool sprayCrown = int(push.params.x + 0.5) == 1 && (floatBitsToUint(FOREST_HEADER(FOREST_H_LOOK)) & 15u) != 0u;
    float past = outHollow > 0.0 ? 0.0 : sprayCrown ? 0.25 : 0.5;
    vec4 clip = L * vec4(s.center + away * (past * s.radius), 1.0);
    // How far a world metre moves the tile's x and y: the lengths of its
    // first two rows.
    vec2 perMetre = vec2(length(vec3(L[0][0], L[1][0], L[2][0])), length(vec3(L[0][1], L[1][1], L[2][1])));
    clip.xy += s.corner * perMetre * vec2(s.radius, max(s.radius, s.radiusUp)) * clip.w;
    gl_Position = clip;
    outUV = s.corner;
    outSeed = s.seed;
}
