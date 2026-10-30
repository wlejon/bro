#version 450
// A volume's (scene.createVolume) one triangle over the whole view, carrying
// each pixel's ray to volume.frag: camera-relative, the eye at the origin, a
// point on a plane of constant view depth, so it interpolates exactly.

#include "common/scene_camera.glsl"

layout(location = 0) out vec3 outRay;
layout(location = 1) flat out int outVolume;   // the volume's place in the frame (its first instance)

void main() {
    outVolume = gl_InstanceIndex;
    vec2 ndc = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vec4 v = camera.invProj * vec4(ndc, 0.5, 1.0);
    outRay = mat3(camera.invView) * (v.xyz / v.w);
}
