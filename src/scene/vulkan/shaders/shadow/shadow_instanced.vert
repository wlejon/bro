#version 450
// Instanced shadow casters: the instance rows under the node's world matrix.
// Built again with SCENE_VEGETATION: the instances bend as in the colour
// pass (mesh_instanced.vert), so the shadows move with them; with a custom
// vertex chunk (CUSTOM_VERTEX, spliced as a caster with CUSTOM_CASTER) its
// userVertex before the instance rows, as mesh_instanced.vert runs it; and
// with SHADOW_CUTOUT for a textured mesh with an alpha cutoff
// (scene_shadow_cutout.glsl).

layout(location = 0) in vec3 inPos;
#if defined(SCENE_VEGETATION) || defined(CUSTOM_CASTER)
layout(location = 1) in vec3 inNormal;   // a leaf folds about its card's normal
#endif
#if defined(CUSTOM_CASTER) || defined(SHADOW_CUTOUT)
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
#endif
#ifdef CUSTOM_CASTER
layout(location = 4) in vec4 inTangent;
#endif
layout(location = 8)  in vec4 inInstRow0;
layout(location = 9)  in vec4 inInstRow1;
layout(location = 10) in vec4 inInstRow2;
#ifdef SHADOW_CUTOUT
layout(location = 11) in vec4 inInstColor;
#endif
#ifdef SCENE_VEGETATION
layout(location = 12) in vec4 inVeg0;
layout(location = 13) in vec4 inVeg1;
layout(location = 14) in vec4 inVeg2;
layout(location = 15) in vec4 inVeg3;
#endif
#if defined(SCENE_VEGETATION) || defined(CUSTOM_CASTER)
#include "common/scene_camera.glsl"
#endif

#include "common/scene_shadow_push.glsl"
#ifdef SCENE_VEGETATION
#include "common/scene_vegetation.glsl"
#endif
#ifdef SHADOW_CUTOUT
#define SHADOW_CUTOUT_VERTEX 1
#include "shadow/scene_shadow_cutout.glsl"
#endif

//__USER_CHUNK__

void main() {
    vec3 pos = inPos;
#ifdef CUSTOM_VERTEX
    vec3 normal = inNormal;
    vec2 uv = inUV;
    userVertex(pos, normal, uv);
#elif defined(SHADOW_CUTOUT)
    vec2 uv = inUV;
#endif
    mat3 R = mat3(vec3(inInstRow0.x, inInstRow1.x, inInstRow2.x),
                  vec3(inInstRow0.y, inInstRow1.y, inInstRow2.y),
                  vec3(inInstRow0.z, inInstRow1.z, inInstRow2.z));
    vec3 trans = vec3(inInstRow0.w, inInstRow1.w, inInstRow2.w);
    vec4 worldPos = casterModel() * vec4(R * pos + trans, 1.0);
#ifdef SCENE_VEGETATION
    mat3 rot;
    vegetationSway(casterModel(), inVeg0, inVeg1, inVeg2, inVeg3, normalize(mat3(casterModel()) * (R * inNormal)),
                   worldPos.xyz, rot);
#endif
    gl_Position = push.lightViewProj * worldPos;
#ifdef SHADOW_CUTOUT
    shadowCutoutOut(uv, inColor.a, inInstColor.a);
#endif
}
