#version 450
// Forest sprite shadow casters: mesh_forest_sprite.vert's discs into an
// atlas tile, facing the light (offset in the tile's clip space), with
// their leafy cutout (shadow_forest.frag).

layout(location = 0) out vec2 outUV;
layout(location = 1) out float outSeed;

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#include "scene_forest_sprite_vertex.glsl"

void main() {
    ForestSprite s;
    // The draw's mode rides in the wind-mask slot (MeshDraw push.extra.z).
    if (!forestSprite(int(push.params.x + 0.5), gl_InstanceIndex, gl_VertexIndex, casterModel(), s)) {
        gl_Position = FOREST_CULLED;
        outUV = vec2(0.0);
        outSeed = 0.0;
        return;
    }
    mat4 L = push.lightViewProj;
    vec4 clip = L * vec4(s.center, 1.0);
    // How far a world metre moves the tile's x and y: the lengths of its
    // first two rows.
    vec2 perMetre = vec2(length(vec3(L[0][0], L[1][0], L[2][0])), length(vec3(L[0][1], L[1][1], L[2][1])));
    clip.xy += s.corner * perMetre * vec2(s.radius, max(s.radius, s.radiusUp)) * clip.w;
    gl_Position = clip;
    outUV = s.corner;
    outSeed = s.seed;
}
