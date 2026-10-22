#version 450
// Grass blades (scene_grass.glsl): an instance a blade of the ring's list in
// the cover's plan (scene_cover_plan.glsl), the ring mesh's first blade its
// vertices, built by the world's sward module (scene_sward_layer.glsl's
// swardBuild). Drawn with mesh.frag's SCENE_GRASS build, which the module's
// swardShade cuts and shades. shadow_grass.vert is the caster.

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

#define SWARD_BUILD 1
#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#include "ground/scene_grass.glsl"
#include "ground/scene_cover_plan.glsl"

void main() {
    SwardVertex b;
    uvec2 e = coverList.entries[gl_InstanceIndex];
    if (!grassBlade(coverCell(e), coverSlot(e), inPos, b)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outVegShade = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    outWorldPos = b.pos;
    gl_Position = camera.viewProj * vec4(b.pos, 1.0);
    outNormal = b.normal;
    outTangent = b.side;
    outBitangent = cross(b.normal, b.side);
    // Where on its card, and the module's own (its SwardFragment's uv, info).
    outUV = b.uv;
    outColor = b.info;
    // Its colour, and the herb bloom it carries (0 none).
    outInstColor = vec4(b.color, b.speck);
    // A blade: its ao, marked (w <= -2) so the fragment stage takes it for a
    // translucent blade with no paler underside, and how far its
    // translucency has faded; and the way the sward stands, by its bulk.
    outVegShade = vec4(b.up * b.bulk, -(2.0 + b.ao + 2.0 * floor(b.far * 15.0 + 0.5)));
}

// The world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl), after
// all they may use.
#include "ground/sward_module.glsl"
#include "ground/herb_module.glsl"
