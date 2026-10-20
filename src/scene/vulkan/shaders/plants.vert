#version 450
// Plants (scene_plants.glsl): an instance a chunk of one plant's elements
// (its leaves, or its trunk and limbs). A leaf's corners are read from its
// pose (plants_pose.comp built it this frame); a tube's vertices are built
// here from its plant and species. No vertex streams. push.extra.x: 0 the
// leaves, 1 the wood; push.extra.y: elements a chunk.

layout(location = 0) out vec3 outWorldPos;
layout(location = 1) out vec3 outLeafNormal;
layout(location = 2) out vec3 outCrownNormal;
layout(location = 3) out vec3 outAlbedo;
layout(location = 4) out vec3 outGlowColor;
layout(location = 5) out vec4 outCrown;
layout(location = 6) out vec4 outShade;
layout(location = 7) out vec2 outInfo;   // x far, y wood
layout(location = 8) out vec2 outLeafUV;

#include "scene_camera.glsl"
#include "scene_mesh_push.glsl"
#define PLANT_POSES_READ
#include "scene_plants.glsl"

void main() {
    bool wood = push.extra.x > 0.5;
    int perElement = wood ? PLANT_TUBE_VERTS : PLANT_LEAF_POINTS;
    int c = gl_VertexIndex % perElement;
    PlantVertex o;
    bool ok;
    if (wood) {
        vec4 chunk = plants.records[int(plants.header[1].y) + gl_InstanceIndex];
        int k = int(chunk.y) + gl_VertexIndex / perElement;
        // (A wood chunk's z: the plant's tubes drawn, the trunk alone far off.)
        ok = float(k) < chunk.z && plantWood(int(chunk.x), k, c, o);
    } else {
        ok = plantPosedLeaf(gl_InstanceIndex * int(plants.header[0].z) + gl_VertexIndex / perElement, c, o);
    }
    if (!ok) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    outWorldPos = o.pos;
    gl_Position = camera.viewProj * vec4(o.pos, 1.0);
    outLeafNormal = o.leafNormal;
    outCrownNormal = o.crownNormal;
    outAlbedo = o.albedo;
    outGlowColor = o.glowColor;
    outCrown = o.crown;
    outShade = o.shade;
    outInfo = vec2(o.far, wood ? 1.0 : 0.0);
    outLeafUV = o.leafUV;
}
