#version 450
// Herb plants (scene_herb.glsl): an instance a plant of the ring's list in
// the cover's plan (scene_cover_plan.glsl), each copy of the card strip in
// the ring mesh one of the plant's cards, which the world's herb module
// builds where it stands (scene_herb_layer.glsl's herbBuild). Drawn with
// mesh.frag's SCENE_HERB build, which hands each fragment to the module's
// herbShade.

#define HERB_BUILD 1

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) out vec4 outColor;      // the module's HerbVertex.a
layout(location = 4) out vec3 outTangent;
layout(location = 5) out vec3 outBitangent;
layout(location = 6) out vec4 outInstColor;
layout(location = 7) out vec4 outVegShade;
layout(location = 8) flat out vec4 outHerbInfo;   // the module's HerbVertex.id, w the card's dither seed
layout(location = 9) out vec4 outHerbSize;        // the module's HerbVertex.b
layout(location = 10) out vec2 outHerbStand;      // the module's HerbVertex.c

#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#include "ground/scene_grass.glsl"
#include "common/scene_vegetation.glsl"
#include "ground/scene_herb.glsl"
#include "ground/scene_cover_plan.glsl"

void main() {
    HerbVertex h;
    if (!herbVertex(coverCell(coverList.entries[gl_InstanceIndex]), gl_VertexIndex, inPos, h)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outVegShade = vec4(0.0, 0.0, 0.0, -1.0);
        outHerbInfo = vec4(0.0);
        outHerbStand = vec2(0.0);
        return;
    }
    outWorldPos = h.pos;
    gl_Position = camera.viewProj * vec4(h.pos, 1.0);
    outNormal = h.normal;
    outTangent = h.side;
    outBitangent = cross(h.normal, h.side);
    outUV = h.uv;
    outColor = h.a;
    outInstColor = vec4(h.tint, 0.0);
    // A leaf (w < 0, not a blade's <= -2): its ao, and the ground's normal
    // the plant's bulk leans toward.
    outVegShade = vec4(h.ground * h.bulk, -clamp(h.ao, 0.02, 1.0));
    outHerbInfo = vec4(h.id, h.seed);
    outHerbSize = h.b;
    outHerbStand = h.c;
}

// The world's herb module (scene_herb_layer.glsl), after all it may use.
#include "ground/herb_module.glsl"
