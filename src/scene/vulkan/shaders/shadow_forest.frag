#version 450
// A forest sprite's shadow (shadow_forest_sprite.vert): the light its
// sphere's leaves stop, 1 - exp(-depth * chord), kept by a dither over
// leaf-patch cells of the sprite (fixed to it, so the shadow does not crawl
// as it sways). The sphere's own leaves shade it analytically (mesh.frag's
// leafSphere), the cast falls on everything else.

layout(location = 0) in vec2 inUV;
layout(location = 1) in float inSeed;
layout(location = 2) flat in float inDepth;
layout(location = 3) flat in float inShare;   // the tier's share of the tree's caster

#include "scene_forest_sprite.glsl"

void main() {
    float r2 = dot(inUV, inUV);
    if (r2 >= 1.0) discard;
    // Half the chord's depth: calibrated against the leaf casters it hands
    // over from, which on the same receivers stop less (a binary cell over a
    // whole lobe overshades the gaps the leaves leave).
    float cover = (1.0 - exp(-inDepth * 0.47 * sqrt(1.0 - r2))) * inShare;
    vec2 o = vec2(inSeed * 97.0, inSeed * 53.0);
    float h = forestSpriteHash(floor(inUV * 9.0 + o));
    if (h >= cover) discard;
}
