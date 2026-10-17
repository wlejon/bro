#version 450
// Herb plants (scene_herb.glsl): one instance per cell of the herbs' grid
// in the draw's ring rectangle, each copy of the card strip in the ring mesh
// one of the plant's cards (its blooms, stems and leaves), built and swayed
// where it stands. Drawn with mesh.frag's SCENE_HERB build, which cuts each
// card to its leaf, stem or bloom (scene_herb_surface.glsl).

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
layout(location = 8) flat out vec4 outHerbInfo;
layout(location = 9) out vec4 outHerbSize;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#include "scene_grass.glsl"
#include "scene_vegetation.glsl"
#include "scene_herb.glsl"

void main() {
    HerbVertex h;
    if (!herbVertex(gl_InstanceIndex, gl_VertexIndex, inPos, h)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        outVegShade = vec4(0.0, 0.0, 0.0, -1.0);
        outHerbInfo = vec4(0.0);
        return;
    }
    outWorldPos = h.pos;
    gl_Position = camera.viewProj * vec4(h.pos, 1.0);
    outNormal = h.normal;
    outTangent = h.side;
    outBitangent = cross(h.normal, h.side);
    outUV = h.uv;
    outColor = vec4(h.info.w, h.size.w, h.down);
    outInstColor = vec4(h.tint, 0.0);
    // A leaf (w < 0, not a blade's <= -2): its ao, and the ground's normal
    // the plant's bulk leans toward.
    outVegShade = vec4(h.ground * h.bulk, -clamp(h.ao, 0.02, 1.0));
    outHerbInfo = vec4(h.info.xyz, 0.0);
    outHerbSize = h.size;
}
