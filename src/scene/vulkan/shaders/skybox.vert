#version 450

// The camera depth policy (scene/vulkan/scene_vk_depth.h).
layout(constant_id = 0) const bool REVERSED_Z = true;

layout(location = 0) out vec2 outUV;

void main() {
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // Just inside the far plane, so anything drawn later is in front of it.
    gl_Position = vec4(outUV * 2.0 - 1.0, REVERSED_Z ? 0.0001 : 0.9999, 1.0);
}
