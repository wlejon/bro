#version 450
// A forest sprite's shadow: its leafy cutout (scene_forest_sprite.glsl).

layout(location = 0) in vec2 inUV;
layout(location = 1) in float inSeed;

#include "scene_forest_sprite.glsl"

void main() {
    if (!forestSpriteCovers(inUV, inSeed)) discard;
}
