#version 450
// The sky passes' full-screen triangle (scene/vulkan/scene_environment.h):
// hands each fragment its world-space view direction, through the camera's
// inverse projection and view, and sits on the far plane.

#include "scene_camera.glsl"

layout(location = 0) out vec3 outWorldDir;

void main() {
    vec2 ndc = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
    // A point on the near plane, in view space: the eye is at the origin.
    vec4 nearPoint = camera.invProj * vec4(ndc, 1.0, 1.0);   // (reversed-Z: near is 1)
    outWorldDir = mat3(camera.invView) * (nearPoint.xyz / nearPoint.w);
    gl_Position = vec4(ndc, 0.0, 1.0);   // at the far plane
}
