#version 450
// Branch tube shadow casters: mesh_tube.vert's walls into a shadow view,
// bending with them when the node carries vegetation records.

#include "common/scene_camera.glsl"
#include "common/scene_shadow_push.glsl"
#include "common/scene_procedural.glsl"
#include "common/scene_vegetation.glsl"

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
        proceduralVegetation(seg, endT, pos, v0, v1, v2, v3);
        mat3 rot;
        vegetationSwayAxis(model, v0, v1, v2, v3, vec3(0.0), vec3(0.0, 1.0, 0.0), worldPos.xyz, rot);
    }
    gl_Position = push.lightViewProj * worldPos;
}
