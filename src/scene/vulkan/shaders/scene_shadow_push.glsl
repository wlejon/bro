// The per-draw push constants of every shadow caster pipeline (PassShadow):
// the atlas tile's light view-projection and the caster's affine world
// matrix as three rows, both camera-relative like the colour pass's.
layout(push_constant) uniform ShadowPushConstants {
    mat4 lightViewProj;
    vec4 modelRows[3];
    // x wind mask; a forest caster's yzw the camera's near sphere (the
    // drawer's Lod::leavesRange, leavesBand) and its side, 0 for none.
    vec4 params;
} push;

#define uWindMask (push.params.x)   // the GL name custom chunks use
// scene_vegetation.glsl: a caster keeps its near leaves whole.
#define VEG_SHADOW_CASTER 1

mat4 casterModel() {
    return transpose(mat4(push.modelRows[0], push.modelRows[1], push.modelRows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}
mat4 sceneModel() { return casterModel(); }

// A forest caster of a tree across the camera's near sphere (mesh.frag's
// MESH_FOREST_REVEAL): the camera draws a pixel as leaves by its distance
// through the band, else as clumps, so a leaf's caster (side > 0) is kept
// with that chance by its own distance and a clump's (side < 0) with the
// rest. (Split by share over the whole crown, the leaves inside took clump
// discs' shade from all over it: a pine's near leaves 8 levels darker.)
bool forestCasterKept(vec3 at, float h) {
    float side = push.params.w;
    if (side == 0.0 || push.params.y <= 0.0) return true;
    float inside = clamp((push.params.y + 0.5 * push.params.z - length(at)) / max(push.params.z, 1e-3), 0.0, 1.0);
    return side > 0.0 ? h < inside : h >= inside;
}
