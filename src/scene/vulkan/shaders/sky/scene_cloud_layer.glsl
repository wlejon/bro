// The cloud deck's module contract (docs/scene-world-architecture.md §13,
// "volumes"; docs/clouds-api.js; scene/world/clouds.h). A world's clouds are
// GLSL that defines
//
//   vec4 cloudCover(vec3 up)
//       the deck's large-scale state at body direction `up` (unit, the
//       body's frame): x how much of the sky its clouds cover there (0 .. 1),
//       y, z, w the world's own (a cloud type, how high the tops reach ...).
//       Run once over a cube about the body when the clouds are set (about
//       10 km a texel, four samples a texel), not every frame: it may be as
//       costly as it likes (FastNoise2 trees' .glsl(), the land's processes'
//       grid through cloudLandGrid). Each of its four values in 0 .. 1.
//   float cloudDensity(CloudPoint q)
//       the deck's extinction (1/m) at a point of it: cumulus about 0.02 to
//       0.1, a thin veil 0.001. Called where the engine marches the deck
//       (every pixel's view through it, the sun's way out of it from each of
//       those samples, the shadow's levels), so it is the costly one: read
//       q.cover (the cover there, bilinear) and the engine's noise
//       (cloudNoise), and keep detail finer than q.footprint at its mean,
//       so what is under a pixel blurs softly and the same every frame. Zero
//       outside the deck is the engine's.
//
// and may read cloudData(i), the world's data (vec4 i of at most 64). The
// engine owns the rest: the march against the scene's depth, the light (the
// sun through the air and through the deck, its forward lobe and its
// multiple scattering, the sky's light), the air before the deck, and the
// deck's shadow on the land.
//
// The includer defines CLOUD_SET, the set this block sits in.

#include "sky/clouds_layout.h"

// The clouds' block (SceneClouds::Block, vulkan/sky/scene_clouds.h).
layout(std140, set = CLOUD_SET, binding = CL_B_UBO) uniform CloudsUBO {
    vec4 toBody[3];     // rows of the place frame's turn into the body's (air space -> body frame)
    vec4 shell;         // x the deck's bottom radius, y its top's, z the datum's radius, w the time (s)
    vec4 params;        // x the scattering albedo, y radians a pixel, z the shortest view step (m), w a step's share of its distance
    vec4 sun;           // xyz toward the sun (air space), w its angular radius
    vec4 shadowN;       // xyz the shadow's nadir (air space), w the levels' growth
    vec4 shadowE1;      // xyz its plane's first axis, w the finest level's texel (m)
    vec4 shadowE2;      // xyz its second axis
    vec4 data[CL_MAX_DATA];
} clouds;

layout(set = CLOUD_SET, binding = CL_B_COVER) uniform sampler2DArray cloudCoverMap;
layout(set = CLOUD_SET, binding = CL_B_NOISE) uniform sampler3D cloudNoiseMap;

struct CloudPoint {
    vec3 p;            // the point in the body's frame (m from its centre)
    vec3 up;           // its direction (unit, the body's frame)
    float height;      // 0 at the deck's bottom .. 1 at its top
    float altitude;    // metres over the datum
    vec4 cover;        // cloudCover at `up` as the cover holds it
    float footprint;   // metres: what is finer than this is at its mean here
    float time;        // the scene's clock (s): what drifts and grows
};

vec4 cloudData(int i) { return clouds.data[clamp(i, 0, CL_MAX_DATA - 1)]; }

// The world's (defined by its module, after this file).
vec4 cloudCover(vec3 up);
float cloudDensity(CloudPoint q);
// The land's processes' grid (ground/land_grid.glsl): channel `c` at body
// direction `up`, for the cover's bake of a deck set with a round land
// (setClouds' `land`); zero anywhere else.
vec4 cloudLandGrid(int c, vec3 up);

// --- The engine's noise ---------------------------------------------------------
//
// One tile of tiling noise (CL_NOISE_SIDE^3): x Perlin-Worley (soft billows
// with round, cellular edges: a cloud's body), y Worley's cells 1 - F1 at 4
// a tile with two finer octaves, z the same at 8 (the detail that erodes a
// cloud's edges), w x squared (its mean, with x's, says how x varies within
// the footprint: cloudNoiseAbove). `x` is the point in tiles (a tile is
// whatever length the world divides by), `footprint` how much of a tile is
// finer than the caller can show: the tile's mips blur it to the mean beyond.
vec4 cloudNoise(vec3 x, float footprint) {
    float lod = log2(max(footprint * float(CL_NOISE_SIDE), 1.0));
    return textureLod(cloudNoiseMap, fract(x), lod);
}

// The share of a lookup's footprint where its x is over `threshold`: x's
// spread there (from w) taken as a normal's. Under a fine footprint it is
// all or nothing, as x is; under a wide one, the share of the sky a field
// of clouds cut from x at that threshold covers, which is what a world's
// deck seen from afar should hold (its mean) so a view or a shadow that
// reads it wide agrees with one that reads it fine.
float cloudNoiseAbove(vec4 n, float threshold) {
    float sd = sqrt(max(n.w - n.x * n.x, 0.0)) + 0.01;
    float z = clamp((n.x - threshold) / sd, -8.0, 8.0);   // (tanh of a large z is NaN on some GPUs)
    // (The normal's CDF, near enough, its tails cut at two and a half
    // spreads: a sky that is clear is clear, not a veil a long view
    // gathers.)
    return clamp((0.5 + 0.5 * tanh(1.1 * z) - 0.004) / 0.992, 0.0, 1.0);
}

// `v` from [lo, hi] onto [a, b], unclamped.
float cloudRemap(float v, float lo, float hi, float a, float b) {
    return a + (v - lo) * (b - a) / max(hi - lo, 1e-6);
}

// --- The cover's cube -----------------------------------------------------------

const vec3 kCloudFaceN[6] = vec3[6](vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0), vec3(0, 0, 1),
                                    vec3(0, 0, -1));
const vec3 kCloudFaceU[6] = vec3[6](vec3(0, 0, -1), vec3(0, 0, 1), vec3(1, 0, 0), vec3(1, 0, 0), vec3(1, 0, 0),
                                    vec3(-1, 0, 0));
const vec3 kCloudFaceV[6] = vec3[6](vec3(0, 1, 0), vec3(0, 1, 0), vec3(0, 0, 1), vec3(0, 0, -1), vec3(0, 1, 0),
                                    vec3(0, 1, 0));
const float CLOUD_QUARTER_PI = 0.78539816;

// Face `f`'s direction at `e`, equal-angle: each of its coordinates -1 .. 1
// across the face.
vec3 cloudFaceDir(int f, vec2 e) {
    vec2 a = tan(e * CLOUD_QUARTER_PI);
    return normalize(kCloudFaceN[f] + a.x * kCloudFaceU[f] + a.y * kCloudFaceV[f]);
}

// The cover at body direction `up`, bilinear. (The face's axes by branch,
// not from the tables: a table indexed by a value the shader works out is
// read from memory on some GPUs, and this runs for every sample.)
vec4 cloudCoverAt(vec3 up) {
    vec3 a = abs(up);
    int f;
    vec3 uvn;
    if (a.x >= a.y && a.x >= a.z) {
        f = up.x >= 0.0 ? 0 : 1;
        uvn = up.x >= 0.0 ? vec3(-up.z, up.y, a.x) : vec3(up.z, up.y, a.x);
    } else if (a.y >= a.z) {
        f = up.y >= 0.0 ? 2 : 3;
        uvn = up.y >= 0.0 ? vec3(up.x, up.z, a.y) : vec3(up.x, -up.z, a.y);
    } else {
        f = up.z >= 0.0 ? 4 : 5;
        uvn = up.z >= 0.0 ? vec3(up.x, up.y, a.z) : vec3(-up.x, up.y, a.z);
    }
    vec2 e = atan(uvn.xy / uvn.z) / CLOUD_QUARTER_PI;
    const float S = float(CL_COVER_SIDE);
    vec2 uv = ((e * 0.5 + 0.5) * (S - 2.0) + 1.0) / S;
    return textureLod(cloudCoverMap, vec3(uv, float(f)), 0.0);
}

// --- The deck --------------------------------------------------------------------

// An air-space point (about the body's centre, the place frame's axes) in
// the body's frame.
vec3 cloudToBody(vec3 a) {
    return vec3(dot(clouds.toBody[0].xyz, a), dot(clouds.toBody[1].xyz, a), dot(clouds.toBody[2].xyz, a));
}

// The deck's extinction (1/m) at air-space point `a`, what is finer than
// `footprint` metres at its mean: zero outside the deck.
float cloudExtinction(vec3 a, float footprint) {
    float r = length(a);
    float h = (r - clouds.shell.x) / (clouds.shell.y - clouds.shell.x);
    if (h <= 0.0 || h >= 1.0) return 0.0;
    vec3 p = cloudToBody(a);
    vec3 up = p / r;
    CloudPoint q = CloudPoint(p, up, h, r - clouds.shell.z, cloudCoverAt(up), footprint, clouds.shell.w);
    return max(cloudDensity(q), 0.0);
}

// Where a ray from `ro` (air space) is inside the deck before `tMax`: at
// most two spans (a ray from above that passes under the deck's bottom
// without meeting the ground comes back up through it); a span's x >= y
// when there is none.
void cloudSpans(vec3 ro, vec3 rd, float tMax, out vec2 a, out vec2 b) {
    a = vec2(1.0, 0.0);
    b = vec2(1.0, 0.0);
    float Rb = clouds.shell.x, Rt = clouds.shell.y;
    float bb = dot(ro, rd), c0 = dot(ro, ro);
    float dt = bb * bb - (c0 - Rt * Rt);
    if (dt <= 0.0) return;
    float st = sqrt(dt);
    vec2 top = vec2(max(-bb - st, 0.0), -bb + st);   // inside the top's sphere
    top.y = min(top.y, tMax);
    if (top.x >= top.y) return;
    float db = bb * bb - (c0 - Rb * Rb);
    if (db <= 0.0) {
        a = top;
        return;
    }
    float sb = sqrt(db);
    vec2 bot = vec2(-bb - sb, -bb + sb);             // inside the bottom's sphere
    // The top's span less the bottom's.
    a = vec2(top.x, min(top.y, bot.x));
    b = vec2(max(top.x, bot.y), top.y);
}
