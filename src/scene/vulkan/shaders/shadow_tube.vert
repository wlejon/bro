#version 450
// Branch tube shadow casters: mesh_tube.vert's walls into an atlas tile.

#include "scene_shadow_push.glsl"
#include "scene_procedural.glsl"

void main() {
    vec3 pos, radial, axisDir;
    vec2 uv;
    if (!tubeVertex(gl_VertexIndex, pos, radial, axisDir, uv)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    gl_Position = push.lightViewProj * (casterModel() * vec4(pos, 1.0));
}
