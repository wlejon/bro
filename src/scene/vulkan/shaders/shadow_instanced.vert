#version 450
// Instanced shadow casters: the instance rows under the node's world matrix.

layout(location = 0) in vec3 inPos;
layout(location = 8)  in vec4 inInstRow0;
layout(location = 9)  in vec4 inInstRow1;
layout(location = 10) in vec4 inInstRow2;

#include "scene_shadow_push.glsl"

void main() {
    mat3 R = mat3(vec3(inInstRow0.x, inInstRow1.x, inInstRow2.x),
                  vec3(inInstRow0.y, inInstRow1.y, inInstRow2.y),
                  vec3(inInstRow0.z, inInstRow1.z, inInstRow2.z));
    vec3 trans = vec3(inInstRow0.w, inInstRow1.w, inInstRow2.w);
    gl_Position = push.lightViewProj * (casterModel() * vec4(R * inPos + trans, 1.0));
}
