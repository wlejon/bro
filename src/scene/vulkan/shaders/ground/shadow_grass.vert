#version 450
// Grass shadow casters: mesh_grass.vert's blades into an atlas tile, bending
// with them. Only the cells the camera draws cast (the same draws and
// instance test), which is all a near ring's self-shadowing needs, and of
// those only a share of the blades, widened to keep their cover: a shadow
// texel is wider than a fine blade.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

#include "common/scene_camera.glsl"
#include "common/scene_shadow_push.glsl"
#define GRASS_SHADOW 1   // a sparser set of wider blades (grass.lod.z)
#include "ground/scene_grass.glsl"

void main() {
    GrassBlade b;
    if (!grassBlade(gl_InstanceIndex, gl_VertexIndex, inPos, inNormal, b)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    gl_Position = push.lightViewProj * vec4(b.pos, 1.0);
}
