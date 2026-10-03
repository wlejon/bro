#version 450

layout(location = 0) out vec2 outUV;

void main() {
    outUV = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // In reversed-Z, z near 0.0 is the far plane
    gl_Position = vec4(outUV * 2.0 - 1.0, 0.0001, 1.0);
}
