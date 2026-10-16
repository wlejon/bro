#version 450
// Forest sprites (scene_forest.glsl), the far tiers: camera-facing discs
// with a leafy cutout (scene_forest_sprite.glsl) and a sphere's normal.
//   mode 0 (push.extra.z), CLUMPS: each instance an entry, a run of its
//          tree's crown clumps, each drawn as the three lobes its leaves
//          fill (procClumpLobe), so the far crown keeps the near crown's
//          masses, outline and colour;
//   mode 1, CROWNS: one sprite a tree, the whole crown, entry
//          gl_VertexIndex / 6 (the draw is not instanced).
// Every sprite is posed by vegPose at its centre (a clump with its weight on
// its limb, a crown by its trunk), so it moves as the leaves it stands in
// for, and carries the gust at its tree (outColor.y) for mesh.frag's sheen:
// a gust turns leaves to show their paler undersides, so gusts roll across
// a distant canopy as lighter bands. shadow_forest_sprite.vert is the caster.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
layout(location = 7) out vec4 outVegShade;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#include "scene_forest_sprite_vertex.glsl"

void main() {
    ForestSprite s;
    if (!forestSprite(int(push.extra.z + 0.5), gl_InstanceIndex, gl_VertexIndex, push.model, s)) {
        gl_Position = FOREST_CULLED;
        outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 right = camera.invView[0].xyz;
    vec3 up = camera.invView[1].xyz;
    vec3 toEye = normalize(-s.center);
    // A crown is wider than tall; seen from above it is round.
    float ry = mix(s.radiusUp, s.radius, abs(toEye.y));
    vec3 pos = s.center + right * (s.corner.x * s.radius) + up * (s.corner.y * ry);

    outWorldPos = pos;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
    outNormal = toEye;
    // Lengths are the radii: mesh.frag lights the sphere's surface.
    outTangent = right * s.radius;
    outBitangent = up * ry;
    outUV = s.corner;
    outColor = vec4(s.seed, s.gust, push.extra.z < 0.5 ? 1.0 : 0.0, 1.0);
    outInstColor = vec4(s.tint, s.fade);
    outVegShade = s.shade;
}
