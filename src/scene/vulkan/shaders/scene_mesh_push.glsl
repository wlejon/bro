// The per-draw push constants of every mesh pipeline (MeshPushConstants,
// scene_mesh_drawer.h) and the flag bits of pbrParams.w.
layout(push_constant) uniform MeshPushConstants {
    mat4 model;
    vec4 baseColor;
    vec4 emissive;   // rgb tint, a intensity
    vec4 pbrParams;  // x metallic, y roughness, z alpha cutoff, w flags
    vec4 extra;      // x near clip, y subsurface, z wind mask, w atlas grid (cols + rows * 256)
} push;

#define uWindMask (push.extra.z)   // the GL name custom chunks use

// The draw's model matrix (the node's world matrix; an instanced draw's
// instances are relative to it). Custom chunks reach it as uModel.
mat4 sceneModel() { return push.model; }

const uint MESH_ALBEDO_MAP          = 1u;
const uint MESH_NORMAL_MAP          = 2u;
const uint MESH_METALLIC_ROUGHNESS  = 4u;
const uint MESH_EMISSIVE_MAP        = 8u;
const uint MESH_UNLIT               = 16u;
const uint MESH_SHADE_MAP           = 32u;
const uint MESH_REFLECTANCE         = 64u;    // alpha carries the SSR reflectance
const uint MESH_OCCLUSION_MAP       = 128u;
const uint MESH_RECEIVES_SHADOW     = 256u;
const uint MESH_TWO_SIDED           = 512u;
const uint MESH_HAS_TANGENTS        = 4096u;
// Bits 10-11: the vertex colour mode — 0 none, 1 replaces the albedo, 2 tints it.
const uint MESH_VERTEX_COLOR_SHIFT  = 10u;

uint meshFlags() { return uint(push.pbrParams.w); }
uint meshVertexColorMode() { return (meshFlags() >> MESH_VERTEX_COLOR_SHIFT) & 3u; }
