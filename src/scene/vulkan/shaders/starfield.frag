#version 450
// Sparse stars on the celestial sphere, added over whichever sky drew
// (atmosphere or environment): a bright day sky swamps them and a near-black
// one lets them through. A hash of the world-space view direction, so they
// stay fixed to the sky as the camera moves; only the rotation turns them.

#include "sky_common.glsl"

float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123); }
vec2  h22(vec2 p) {
    return fract(sin(vec2(dot(p, vec2(127.1, 311.7)),
                          dot(p, vec2(269.5, 183.3)))) * 43758.5453123);
}

// Unit direction -> a 2D grid coordinate on the dominant cube face. `salt`
// decorrelates the six faces so opposite faces don't mirror the same pattern.
// Seam-exact continuity across face edges isn't needed: stars are points and a
// dropped one at an edge is invisible among thousands.
vec2 faceGrid(vec3 d, float grid, out float salt) {
    vec3 a = abs(d);
    vec2 uv;
    if (a.x >= a.y && a.x >= a.z)      { uv = d.yz / a.x; salt = d.x > 0.0 ? 1.0 : 2.0; }
    else if (a.y >= a.z)               { uv = d.xz / a.y; salt = d.y > 0.0 ? 3.0 : 4.0; }
    else                               { uv = d.xy / a.z; salt = d.z > 0.0 ? 5.0 : 6.0; }
    return (uv * 0.5 + 0.5) * grid;
}

// One grid resolution's worth of stars. `thresh` is the fraction of cells lit;
// `size` sets the point radius in grid units. Scans the 3x3 neighbourhood so a
// star whose centre sits in an adjacent cell still lights this pixel.
vec3 starLayer(vec3 d, float grid, float thresh, float size) {
    float salt;
    vec2 g = faceGrid(d, grid, salt);
    vec2 cell = floor(g);
    vec3 col = vec3(0.0);
    for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i) {
        vec2 c  = cell + vec2(float(i), float(j));
        vec2 cs = c + salt * 17.0;
        if (h21(cs) > thresh) continue;                 // most cells are empty sky
        vec2 pos = c + 0.2 + 0.6 * h22(cs + 1.7);       // jittered position in-cell
        float mag = h21(cs + 5.3);                      // apparent magnitude
        float b = smoothstep(size * (0.5 + mag), 0.0, length(g - pos))
                * (0.2 + 0.8 * mag * mag);
        float t = h21(cs + 9.1);                        // colour temperature
        vec3 tint = mix(vec3(1.0, 0.83, 0.66), vec3(0.74, 0.85, 1.0), t);
        col = max(col, b * tint);
    }
    return col;
}

void main() {
    outIndirect = vec4(0.0);
    vec3 d = rotateY(normalize(inWorldDir), sky.params.z);
    float thresh = clamp(0.05 * sky.params.y, 0.0, 0.9);
    // Two layers: a sparse bright field and a denser faint one for depth.
    vec3 col = starLayer(d, 90.0, thresh, 0.06) + starLayer(d, 165.0, thresh * 0.7, 0.04) * 0.6;
    outColor = vec4(col * sky.params.x, 1.0);
}
