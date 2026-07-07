// Billboard shaders — a single camera-facing textured/filled quad. The
// vertex shader places a unit quad at a world anchor using camera basis
// vectors supplied by the CPU. Positions are computed in a *camera-relative*
// frame (anchor - cameraEye), which matches renderMeshNode and avoids float
// precision loss at large world coordinates.

#version 330 core
layout(location = 0) in vec2 aQuad;  // [-1..1] corner

uniform mat4 uVP;            // projection * viewRot (no translation)
uniform vec3 uAnchorRel;     // worldAnchor - cameraEye
uniform vec3 uRight;         // billboard right axis, world space
uniform vec3 uUp;            // billboard up axis, world space
uniform vec2 uHalfSize;      // world-space half-extents
uniform vec2 uUvMin;         // texture sub-rect (default 0,0)
uniform vec2 uUvMax;         // texture sub-rect (default 1,1)

out vec2 vUV;        // [0..1] within the local quad
out vec2 vTexUV;     // sampler UV: lerp(uvMin, uvMax, vUV)

void main() {
    vec3 worldRel = uAnchorRel
                  + uRight * (aQuad.x * uHalfSize.x)
                  + uUp    * (aQuad.y * uHalfSize.y);
    // Flip Y so UV origin is top-left (matches Skia/CSS pixel layout).
    vUV = vec2(aQuad.x * 0.5 + 0.5, 0.5 - aQuad.y * 0.5);
    vTexUV = mix(uUvMin, uUvMax, vUV);
    gl_Position = uVP * vec4(worldRel, 1.0);
}
