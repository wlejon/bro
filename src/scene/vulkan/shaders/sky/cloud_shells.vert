#version 450
// The shells (vulkan/sky/scene_clouds.cpp; sky/clouds_layout.h's CL_SHELL_*):
// a heap an instance, drawn far to near as the box about its lobes, a
// couple of pixels wider for its edge's ease; the fragments trace the lobes
// (sky/cloud_shells.frag). (The lighting set is the fragments' alone: none
// of it is read here.)

#include "common/scene_camera.glsl"
#define SCENE_VERTEX_STAGE 1
#include "common/scene_lighting.glsl"

#define CLOUD_SET 2
#include "sky/scene_cloud_layer.glsl"
#include "sky/cloud_shells.glsl"

layout(location = 0) out vec3 outPos;            // camera-relative
layout(location = 1) flat out int outHeap;

// A box's twelve triangles over its corners (x 1, y 2, z 4).
const int kBox[36] = int[36](0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3, 0, 4, 5, 0, 5, 1,
                             2, 3, 7, 2, 7, 6, 0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5);

void main() {
    ClShell h = clShell(gl_InstanceIndex);
    vec3 lo, hi;
    clShellBox(h, clShellGrow(h.dist) + 2.0 * clouds.params.y * h.dist, lo, hi);
    int c = kBox[gl_VertexIndex];
    vec3 s = vec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
    vec3 b = mix(lo, hi, s);
    vec3 pos = h.centre + h.east * b.x + h.north * b.y + h.up * b.z;
    outPos = pos;
    outHeap = gl_InstanceIndex;
    gl_Position = camera.viewProj * vec4(pos, 1.0);
}
