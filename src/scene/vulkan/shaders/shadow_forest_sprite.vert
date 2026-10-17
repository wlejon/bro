#version 450
// Forest sprite shadow casters: mesh_forest_sprite.vert's spheres as discs
// into an atlas tile, facing the light (offset in the tile's clip space),
// as much of the light as their leaves stop (shadow_forest.frag).

layout(location = 0) out vec2 outUV;
layout(location = 1) out float outSeed;
layout(location = 2) flat out float outDepth;   // the leaves' optical depth through the middle

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
        return;
    }
    float r = max(s.radius, 1e-6);
    outDepth = forestMassDepth(sqrt(6.2831853 / s.leaves), s.leafLen / r, procedural.header[3].y);
    mat4 L = push.lightViewProj;
    // The disc stands half a radius past the centre, away from the light
    // (the tile's depth grows that way): the light reaching a leaf in the
    // sphere has crossed only the leaves above it, so the disc shades the
    // sphere's far part and what lies beyond, not its lit side. (A sprite
    // takes its own leaves' shade analytically, mesh.frag's leafSphere.)
    vec3 away = normalize(vec3(L[0][2], L[1][2], L[2][2]));
    vec4 clip = L * vec4(s.center + away * (0.5 * s.radius), 1.0);
    // How far a world metre moves the tile's x and y: the lengths of its
    // first two rows.
    vec2 perMetre = vec2(length(vec3(L[0][0], L[1][0], L[2][0])), length(vec3(L[0][1], L[1][1], L[2][1])));
    clip.xy += s.corner * perMetre * vec2(s.radius, max(s.radius, s.radiusUp)) * clip.w;
    gl_Position = clip;
    outUV = s.corner;
    outSeed = s.seed;
}
