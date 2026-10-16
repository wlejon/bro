#version 450
// Instanced shadow casters: the instance rows under the node's world matrix.
// Built again with SCENE_VEGETATION: the instances bend as in the colour
// pass (mesh_instanced.vert), so the shadows move with them.

layout(location = 0) in vec3 inPos;
layout(location = 8)  in vec4 inInstRow0;
layout(location = 9)  in vec4 inInstRow1;
layout(location = 10) in vec4 inInstRow2;
#ifdef SCENE_VEGETATION
layout(location = 12) in vec4 inVeg0;
layout(location = 13) in vec4 inVeg1;
layout(location = 14) in vec4 inVeg2;
layout(location = 15) in vec4 inVeg3;
#include "scene_camera.glsl"
#endif

#include "scene_shadow_push.glsl"
#ifdef SCENE_VEGETATION
#include "scene_vegetation.glsl"
#endif

void main() {
    mat3 R = mat3(vec3(inInstRow0.x, inInstRow1.x, inInstRow2.x),
                  vec3(inInstRow0.y, inInstRow1.y, inInstRow2.y),
                  vec3(inInstRow0.z, inInstRow1.z, inInstRow2.z));
    vec3 trans = vec3(inInstRow0.w, inInstRow1.w, inInstRow2.w);
    vec4 worldPos = casterModel() * vec4(R * inPos + trans, 1.0);
#ifdef SCENE_VEGETATION
    mat3 rot;
    vegetationSway(casterModel(), inVeg0, inVeg1, inVeg2, inVeg3, worldPos.xyz, rot);
#endif
    gl_Position = push.lightViewProj * worldPos;
}
