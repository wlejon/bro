#version 450
// Depth-only shadow casters into one atlas tile, with the same wind sway and
// custom vertex hook as mesh.vert so the silhouette matches the colour pass.
// shadow_skinned.vert deforms with the bone palette first.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec4 inColor;
#ifdef CUSTOM_CASTER
// A custom vertex chunk may read the tangent (aTangent), as in the colour pass.
layout(location = 4) in vec4 inTangent;
#endif

#include "scene_camera.glsl"
#include "scene_shadow_push.glsl"

//__USER_CHUNK__

void main() {
    mat4 model = casterModel();
    vec3 pos = inPos;
    vec3 normal = inNormal;
    vec2 uv = inUV;
    pos += inverse(mat3(model)) * windDelta((model * vec4(pos, 1.0)).xyz, inColor.r * push.params.x);
#ifdef CUSTOM_VERTEX
    userVertex(pos, normal, uv);
#endif
    gl_Position = push.lightViewProj * (model * vec4(pos, 1.0));
}
