// The per-draw push constants of every shadow caster pipeline (PassShadow):
// the atlas tile's light view-projection and the caster's affine world
// matrix as three rows, both camera-relative like the colour pass's.
layout(push_constant) uniform ShadowPushConstants {
    mat4 lightViewProj;
    vec4 modelRows[3];
    // x wind mask (a plants caster's mode); yzw a cutout caster's
    // (scene_shadow_cutout.glsl).
    vec4 params;
} push;

#define uWindMask (push.params.x)   // the GL name custom chunks use
// scene_vegetation.glsl: a caster keeps its near leaves whole.
#define VEG_SHADOW_CASTER 1

mat4 casterModel() {
    return transpose(mat4(push.modelRows[0], push.modelRows[1], push.modelRows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}
mat4 sceneModel() { return casterModel(); }
