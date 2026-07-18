#version 330 core
// Decal box vertex shader. The geometry is the unit cube [-0.5, 0.5]^3; the
// node's world transform (camera-relative, like every other pass) places and
// sizes the projection volume. All the interesting work happens in the
// fragment shader, which reconstructs the opaque scene position from the
// depth snapshot — the box merely rasterizes the screen area the volume can
// touch. The pass draws BACK faces with depth-test GEQUAL so a camera inside
// the volume still rasterizes it, while pixels where the scene is entirely
// behind the box are skipped early.
// NOTE: #version must stay on line 1 (NVIDIA pre-scan gotcha, see mesh.vert).

layout(location = 0) in vec3 aPos;

uniform mat4 uMVP;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
}
