#version 450
// Grass blades (scene_grass.glsl): an instance a blade of the ring's list in
// the cover's plan (scene_cover_plan.glsl), the ring mesh's first blade its
// vertices, bent into place by the wind and the movers. Drawn with mesh.frag's
// SCENE_GRASS build: a leaf folded along its midrib near the eye, its normal
// leaning toward the ground's further off, the sun glowing through it from
// behind, the root in shade, a culm's head cut per pixel.
// shadow_grass.vert is the caster.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
layout(location = 7) out vec4 outVegShade;

#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#include "ground/scene_grass.glsl"
#include "ground/scene_cover_plan.glsl"

void main() {
    GrassBlade b;
    uvec2 e = coverList.entries[gl_InstanceIndex];
    if (!grassBlade(coverCell(e), coverSlot(e), inPos, inNormal, b)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    outWorldPos = b.pos;
    gl_Position = camera.viewProj * vec4(b.pos, 1.0);
    outNormal = b.normal;
    outTangent = b.side;
    outBitangent = cross(b.normal, b.side);
    // Across (in the blade's width) and along it; and what it is (a leaf, a
    // culm and its head, a dead leaf), for mesh.frag's SCENE_GRASS build.
    outUV = b.uv;
    outColor = b.info;
    outInstColor = vec4(b.color, 0.0);
    // A blade: its ao, marked (w <= -2) so the fragment stage takes it for a
    // translucent blade with no paler underside; and the ground's normal.
    outVegShade = vec4(b.ground * b.bulk, -(2.0 + b.ao + 2.0 * floor(b.far * 15.0 + 0.5)));
}
