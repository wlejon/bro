// The per-draw push constants of every mesh pipeline (MeshPushConstants,
// scene_mesh_drawer.h) and the flag bits of pbrParams.w.
layout(push_constant) uniform MeshPushConstants {
    mat4 model;      // the node's world matrix, camera-relative (eye subtracted)
    vec4 baseColor;
    vec4 emissive;   // rgb tint, a intensity
    vec4 pbrParams;  // x metallic, y roughness, z alpha cutoff, w flags
    vec4 extra;      // x near clip, y subsurface, z wind mask, w atlas grid (cols + rows * 256)
} push;

#define uWindMask (push.extra.z)   // the GL name custom chunks use

// The draw's camera-relative model matrix (the node's world matrix less the
// eye; an instanced draw's instances are relative to it). Custom chunks reach
// it as uModel.
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
// A draw into a 4x MSAA target: a cut card's cover splits samples
// (scene_cover_mask.glsl's coverSampleMask).
const uint MESH_MSAA                = 32768u;
// A grass node's herb plants (mesh_herb.vert, mesh.frag's SCENE_HERB build).
const uint MESH_HERB                = 2097152u;
// The cover's far tier (mesh_cover_far.vert, cover_far.frag).
const uint MESH_COVER_FAR           = 4194304u;
// A land carrying its plants' canopy: the instance colour is the canopy
// there, its leaves' albedo and how much of it is shown (mesh_land.vert).
const uint MESH_CANOPY              = 8388608u;
// Bits 10-11: the vertex colour mode — 0 none, 1 replaces the albedo, 2 tints it.
const uint MESH_VERTEX_COLOR_SHIFT  = 10u;

uint meshFlags() { return uint(push.pbrParams.w); }
uint meshVertexColorMode() { return (meshFlags() >> MESH_VERTEX_COLOR_SHIFT) & 3u; }
