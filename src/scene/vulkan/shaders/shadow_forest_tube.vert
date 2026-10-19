#version 450
// Forest bark shadow casters: mesh_forest_tube.vert's walls into an atlas
// tile, bending with them.

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"
#include "scene_forest.glsl"
#include "scene_forest_tube.glsl"

void main() {
    forestApplyLook();
    ForestEntry e = forestEntry(gl_InstanceIndex);
    vec3 pos, radial, axisDir;
    vec2 uv;
    int seg;
    float endT;
    if (!forestTubeVertex(e, gl_VertexIndex, pos, radial, axisDir, uv, seg, endT)) {
        gl_Position = FOREST_CULLED;
        return;
    }
    int specimen;
    float treeSeed;
    mat4 model = casterModel() * forestInstance(e.instance, specimen, treeSeed);
    vec4 world = model * vec4(pos, 1.0);
    vec4 v0, v1, v2, v3;
    forestVegetation(forestSegmentRecord(seg) + FOREST_SEG_VEGETATION / 4, pos, endT, v0, v1, v2, v3);
    mat3 rot;
    vec3 p = world.xyz;
    vegetationSwayAxis(model, v0, v1, v2, v3, vec3(0.0), vec3(0.0, 1.0, 0.0), p, rot);
    gl_Position = push.lightViewProj * vec4(p, 1.0);
}
