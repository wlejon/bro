// The per-draw push constants of every shadow caster pipeline (PassShadow):
// the atlas tile's light view-projection and the caster's affine world
// matrix as three rows.
layout(push_constant) uniform ShadowPushConstants {
    mat4 lightViewProj;
    vec4 modelRows[3];
    vec4 params;   // x wind mask
} push;

#define uWindMask (push.params.x)   // the GL name custom chunks use

mat4 casterModel() {
    return transpose(mat4(push.modelRows[0], push.modelRows[1], push.modelRows[2], vec4(0.0, 0.0, 0.0, 1.0)));
}
