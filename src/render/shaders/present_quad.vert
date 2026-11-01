#version 450

// Fullscreen triangle from gl_VertexIndex alone (no vertex buffer): the
// presenter draws three vertices covering the target, UV (0,0) top-left.
layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
