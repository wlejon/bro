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
// A forest draw (scene_forest.glsl): inInstColor.a is its tier's dithered
// cross-fade code, not an atlas cell.
const uint MESH_FOREST              = 8192u;
// A forest sprite: a lobe or crown card whose leaves mesh.frag places
// (scene_forest_mass.glsl).
const uint MESH_FOREST_SPRITE       = 16384u;
// A forest draw into a 4x MSAA target: its cross-fades split samples
// (mesh.frag's forestSampleMask).
const uint MESH_FOREST_MSAA         = 32768u;
// Forest leaf clusters: a thinned crown's kept leaf drawn as the leaves of
// its patch (mesh_forest_leaf.vert, scene_forest_mass.glsl).
const uint MESH_FOREST_CLUSTER      = 65536u;
// A broad forest leaf near (mesh_forest_leaf.vert): outUV.x is across its
// card from its own position, and mesh.frag cuts its outline per pixel.
const uint MESH_FOREST_LEAF_CUT     = 1048576u;
// A grass node's herb plants (mesh_herb.vert, mesh.frag's SCENE_HERB build).
const uint MESH_HERB                = 2097152u;
// A forest's leaves (leaf, card, cluster, core and twig draws) or clumps, cut
// to their side of the near sphere (ForestNode::Lod::leavesRange): its
// radius and band in push.emissive.xy (mesh.frag's forestSampleMask).
const uint MESH_FOREST_REVEAL       = 8388608u;
// Bits 10-11: the vertex colour mode — 0 none, 1 replaces the albedo, 2 tints it.
const uint MESH_VERTEX_COLOR_SHIFT  = 10u;

uint meshFlags() { return uint(push.pbrParams.w); }
uint meshVertexColorMode() { return (meshFlags() >> MESH_VERTEX_COLOR_SHIFT) & 3u; }
