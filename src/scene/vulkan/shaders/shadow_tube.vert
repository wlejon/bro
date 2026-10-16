#version 450
// Branch tube shadow casters: mesh_tube.vert's walls into an atlas tile,
// bending with them when the node carries vegetation records.

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"
#include "scene_vegetation.glsl"

void main() {
    vec3 pos, radial, axisDir;
    vec2 uv;
    int seg;
    float endT;
    if (!tubeVertex(gl_VertexIndex, pos, radial, axisDir, uv, seg, endT)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    mat4 model = casterModel();
    vec4 worldPos = model * vec4(pos, 1.0);
    if (proceduralHasVegetation()) {
        vec4 v0, v1, v2, v3;
        proceduralVegetation(seg, endT, pos, vec3(0.0), v0, v1, v2, v3);
        mat3 rot;
        vegetationSway(model, v0, v1, v2, v3, worldPos.xyz, rot);
    }
    gl_Position = push.lightViewProj * worldPos;
}
