#version 450
// A tile of the turf's surface layer (scene_ground_layers.glsl), filled by
// drawing the ground's meshes straight down into it (mesh.vert's static
// build under an orthographic camera fitted to the tile and its border;
// scene_surface_layers.cpp): at each texel, scene_ground.glsl's GroundBake
// of the ground's data there, band-limited to the texel.

layout(location = 0) in vec3 inWorldPos;
layout(location = 3) in vec4 inColor;

layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;
layout(location = 2) out vec4 outC;
layout(location = 3) out vec4 outD;

#define GROUND_LAYER_FILL 1
#include "common/scene_camera.glsl"
#include "common/scene_mesh_push.glsl"
#include "ground/scene_grass.glsl"
#include "ground/scene_ground.glsl"

void main() {
    vec2 xz = inWorldPos.xz + camera.eyeWorld.xz;
    // A texel's side (the camera looks straight down: x and z each step one
    // texel a pixel).
    vec2 w = fwidth(inWorldPos.xz);
    float fp = max(max(w.x, w.y), 1e-4);
    GroundData g = groundData(xz, inColor);
    GroundBake b = groundBake(xz, g, fp);
    outA = b.a;
    outB = b.b;
    outC = b.c;
    outD = b.d;
}

// The world's modules (scene_sward_layer.glsl, scene_herb_layer.glsl), after
// all they may use.
#include "ground/sward_module.glsl"
#include "ground/herb_module.glsl"
