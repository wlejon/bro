// Equirectangular HDR -> cubemap conversion. Uses the shared NDC-quad vertex
// shader (env_convert.vert); this fragment shader is invoked once per cube
// face. uFace selects the face mapping; gl_FragCoord drives the [-1,1]
// surface coords.

#version 330 core
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uEquirect;
uniform int       uFace;       // 0..5 = +X -X +Y -Y +Z -Z

const float PI     = 3.14159265358979;
const float TWO_PI = 6.28318530717958;

// Cubemap face → world-space direction. Standard GL cube convention.
// uv is in [-1,1] (centre of face = 0,0).
vec3 cubeDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));  // +X
    if (face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));  // -X
    if (face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));  // +Y
    if (face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));  // -Y
    if (face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));  // +Z
    return            normalize(vec3(-uv.x, -uv.y, -1.0));      // -Z
}

void main() {
    vec2 uv = vUV * 2.0 - 1.0;
    vec3 d  = cubeDir(uFace, uv);
    float phi   = atan(d.z, d.x);
    float theta = asin(clamp(d.y, -1.0, 1.0));
    // HDRIs from broimage are uploaded top-down (row 0 = north pole),
    // so the +Y direction must read v=0. Hence 0.5 - theta/PI.
    vec2 eq = vec2(phi / TWO_PI + 0.5, 0.5 - theta / PI);
    vec3 s = texture(uEquirect, eq).rgb;
    // Sanitise the source: some HDRs carry tiny negative pixels (from upstream
    // tonemapping) and stray non-finite values (from floating-point overflow
    // in broken encoders). Both poison every downstream convolution pass.
    s = max(s, vec3(0.0));
    if (any(isnan(s)) || any(isinf(s))) s = vec3(0.0);
    FragColor = vec4(s, 1.0);
}
