#version 450
// Depth-only shadow casters into one atlas tile, with the same wind sway and
// custom vertex hook as mesh.vert so the silhouette matches the colour pass.
// shadow_skinned.vert deforms with the bone palette first. Built again with
// SCENE_VEGETATION for meshes carrying vegetation records (mesh.vert's).

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
#ifdef CUSTOM_CASTER
// A custom vertex chunk may read the tangent (aTangent), as in the colour pass.
layout(location = 4) in vec4 inTangent;
#endif
#ifdef SCENE_VEGETATION
layout(location = 12) in vec4 inVeg0;
layout(location = 13) in vec4 inVeg1;
layout(location = 14) in vec4 inVeg2;
layout(location = 15) in vec4 inVeg3;
#endif

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"
#ifdef SCENE_VEGETATION
#include "scene_vegetation.glsl"
#endif

//__USER_CHUNK__

void main() {
    mat4 model = casterModel();
    vec3 pos = inPos;
    vec3 normal = inNormal;
    vec2 uv = inUV;
#ifndef SCENE_VEGETATION
    pos += inverse(mat3(model)) * windDelta((model * vec4(pos, 1.0)).xyz, inColor.r * push.params.x);
#endif
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif
    vec4 worldPos = model * vec4(pos, 1.0);
#ifdef SCENE_VEGETATION
    mat3 rot;
    vegetationSway(model, inVeg0, inVeg1, inVeg2, inVeg3, normalize(mat3(model) * normal), worldPos.xyz, rot);
#endif
    gl_Position = push.lightViewProj * worldPos;
}
