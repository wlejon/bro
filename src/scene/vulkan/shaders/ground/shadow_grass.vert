#version 450
// Grass shadow casters: mesh_grass.vert's blades into a shadow view, bending
// with them. Only the cells the camera draws cast (the same draws and
// instance test), which is all a near ring's self-shadowing needs, and of
// those only a share of the blades, widened to keep their cover: a shadow
// texel is wider than a fine blade.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

#include "common/scene_camera.glsl"
#include "common/scene_shadow_push.glsl"
#define GRASS_SHADOW 1   // a sparser set of wider blades (grass.lod.z)
#define SWARD_BUILD 1
#include "ground/scene_grass.glsl"

void main() {
    // An instance a cell of the ring's caster runs, each copy of the blade
    // in the ring mesh one of its blades.
    SwardVertex b;
    int vpb = max(int(grass.shape.y + 0.5), 1);
    if (!grassBlade(grassRectCell(gl_InstanceIndex), gl_VertexIndex / vpb, inPos, b)) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    gl_Position = push.lightViewProj * vec4(b.pos, 1.0);
}

// The world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl), after
// all they may use.
#include "ground/sward_module.glsl"
#include "ground/herb_module.glsl"
