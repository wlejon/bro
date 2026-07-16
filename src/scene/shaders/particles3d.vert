// 3D particle vertex shader — one camera-facing quad per instance. The
// instance stream carries sim-space position, size, color, roll rotation and
// flipbook frame; uModel lifts local-space sims into world space (identity
// for world-space sims) and positions are made camera-relative on the GPU
// (same precision trick as the mesh/instanced/billboard passes).

#version 330 core
layout(location = 0) in vec2 aQuad;    // [-1..1] corner
layout(location = 1) in vec3 aPos;     // sim-space particle center
layout(location = 2) in float aSize;   // world-space quad size (diameter)
layout(location = 3) in vec4 aColor;   // sRGB-encoded straight-alpha tint
layout(location = 4) in float aRot;    // roll around the view axis (radians)
layout(location = 5) in float aFrame;  // flipbook frame index (continuous)

uniform mat4 uVP;         // projection * viewRot (no translation)
uniform mat4 uModel;      // node world matrix for local sims, else identity
uniform vec3 uCameraEye;
uniform vec3 uRight;      // camera right, world space
uniform vec3 uUp;         // camera up, world space
uniform vec2 uFlipGrid;   // flipbook cols, rows; (1,1) = whole texture

out vec2 vUV;
out vec4 vColor;

void main() {
    float c = cos(aRot);
    float s = sin(aRot);
    vec2 corner = vec2(aQuad.x * c - aQuad.y * s,
                       aQuad.x * s + aQuad.y * c);
    vec3 world = (uModel * vec4(aPos, 1.0)).xyz;
    float halfSize = aSize * 0.5;
    vec3 rel = world - uCameraEye
             + uRight * (corner.x * halfSize)
             + uUp    * (corner.y * halfSize);

    // UV origin top-left (matches image pixel layout), before flipbook remap.
    vec2 uv = vec2(aQuad.x * 0.5 + 0.5, 0.5 - aQuad.y * 0.5);
    if (uFlipGrid.x > 1.0 || uFlipGrid.y > 1.0) {
        int cols = int(uFlipGrid.x);
        int rows = int(uFlipGrid.y);
        int frame = clamp(int(aFrame), 0, cols * rows - 1);
        int cx = frame - (frame / cols) * cols;
        int cy = frame / cols;
        uv = (vec2(float(cx), float(cy)) + uv) / vec2(float(cols), float(rows));
    }
    vUV = uv;
    vColor = aColor;
    gl_Position = uVP * vec4(rel, 1.0);
}
