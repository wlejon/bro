#version 450
// A cutout caster's fragment (scene_shadow_cutout.glsl): the depth of what
// the colour pass keeps, the rest of the card discarded.

#define SHADOW_CUTOUT_FRAGMENT 1
#include "shadow/scene_shadow_cutout.glsl"

void main() {
    shadowCutoutTest();
}
