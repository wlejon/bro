// Starfield: sparse stars painted on the celestial sphere, ADDITIVE over
// whichever sky drew (atmosphere or cubemap). A bright day sky swamps them and
// the near-black space sky lets them through — both correct with one pass.
//
// Stars are a hash of the WORLD-space view direction, so they are fixed to the
// sky and immune to camera translation: flying to "orbit" (a bounded box, in
// this app) does not make them swim. Only uRotation slowly turns the whole
// celestial sphere about Y. Shares skybox.vert's fullscreen ray.

#version 330 core
in vec3 vWorldDir;
out vec4 FragColor;

uniform float uIntensity;   // overall brightness multiplier
uniform float uDensity;     // ~0..2, fraction of grid cells that hold a star
uniform float uRotation;    // Y-axis rotation of the celestial sphere (radians)

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
    vec3 d = normalize(vWorldDir);
    float c = cos(uRotation), s = sin(uRotation);
    d = vec3(c * d.x + s * d.z, d.y, -s * d.x + c * d.z);

    float thresh = clamp(0.05 * uDensity, 0.0, 0.9);
    // Two layers: a sparse bright field and a denser faint one for depth.
    vec3 col = starLayer(d, 90.0,  thresh,       0.06)
             + starLayer(d, 165.0, thresh * 0.7, 0.04) * 0.6;

    FragColor = vec4(col * uIntensity, 1.0);
}
