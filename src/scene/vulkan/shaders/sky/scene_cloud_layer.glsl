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
//       the deck's base at a point of it, 0 .. 1: how much of the air about
//       it is cloud, over the footprint q.footprint. Where that is finer
//       than the clouds, the clouds' own shapes: 0 in the clear air between
//       them, 1 within, the surface where it crosses a half, rising from 0
//       to 1 over about the forms' `edge` metres across it, and never
//       faster any way (the least of that and a flat base's rise, up or
//       down CLOUD_EDGE_STEEPEST times as fast: the skin moves it less);
//       under 0 in the clear air, as far below as the nearest cloud is
//       edges off, so the march steps across it in long strides; all of
//       which the march's steps through clear air trust (the heaps and
//       slabs are the world's; the engine
//       moves that surface a bounded skin in and out, CLOUD_SKIN of the
//       base either way, and opens no hole deeper). Where the footprint is
//       wider than the clouds, the share of
//       it they fill. Baked into the cache about the eye (each texel at its
//       cell's footprint, at least 62.5 m), never run per pixel, so it may
//       read q.cover, the engine's noise (cloudNoise) and the world's data
//       as it likes. Zero outside the deck is the engine's.
//
// and may read cloudData(i), the world's data (vec4 i of at most 64). The
// engine owns the rest: the forms the base is cut into (cloudForms: its
// billows and their eroded edges, the cauliflower over a cloud's middle and
// the wisps under its base, by the world's scales and strength), the march
// against the scene's depth, the light (the sun through the air and through
// the deck, its forward lobe and its multiple scattering, the sky's light),
// the air before the deck, and the deck's shadow on the land.
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
    vec4 cacheC;        // xyz the cache's chart's centre (the body's frame), w the deck's bottom radius
    vec4 cacheE1;       // xyz its first axis (the body's frame), w a bin's height (m)
    vec4 cacheE2;       // xyz its second axis
    vec4 cacheNadir;    // xy the eye's nadir on the chart (m)
    vec4 cacheLevel[CL_CACHE_LEVELS];   // xy the cell (index) of level k's window that holds the nadir
    vec4 forms;         // x a cloud's heart's extinction (1/m), y the billows' tile (m), z the detail's, w the edge (m)
    vec4 wind;          // xyz the wind (m/s, the body's frame): what the forms drift with
    vec4 data[CL_MAX_DATA];
} clouds;

layout(set = CLOUD_SET, binding = CL_B_COVER) uniform sampler2DArray cloudCoverMap;
layout(set = CLOUD_SET, binding = CL_B_NOISE) uniform sampler3D cloudNoiseMap;
layout(set = CLOUD_SET, binding = CL_B_CACHE) uniform sampler3D cloudCacheMap;
layout(set = CLOUD_SET, binding = CL_B_SIGMA) uniform sampler3D cloudSigmaMap;
layout(set = CLOUD_SET, binding = CL_B_BAND) uniform sampler2DArray cloudBandMap;

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
// with round, cellular edges), y Worley's cells 1 - F1 at 4 a tile with two
// finer octaves, z the same at 8, w x squared (its mean, with x's, says how
// x varies within the footprint). `x` is the point in tiles (a tile is
// whatever length the world divides by), `footprint` how much of a tile is
// finer than the caller can show: the tile's mips blur it to the mean beyond.
vec4 cloudNoise(vec3 x, float footprint) {
    float lod = log2(max(footprint * float(CL_NOISE_SIDE), 1.0));
    return textureLod(cloudNoiseMap, fract(x), lod);
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

// --- The forms ---------------------------------------------------------------------
//
// The world's base makes the clouds (its heaps and slabs: their surface
// where it crosses a half); the engine moves that surface a skin in and
// out: the billows' lobes (its Perlin-Worley noise, `forms.billowScale` a
// tile) push it out and in, and the finer Worley detail erodes it into
// cauliflower over a cloud's middle and wisps under its base, by at most
// CLOUD_SKIN of the base either way, so the skin is about 2 CLOUD_SKIN
// `forms.edge` metres deep and no hole opens deeper (a point whose base is
// over a half by more than CLOUD_SKIN is cloud, one under it by more is
// clear air). Under a pixel the surface blurs: the share of the footprint
// the cloud fills ramps over the base the footprint spans, the skin fading
// with it, so a footprint as wide as a cloud's edge or wider reads the base
// itself as the share (what the world's base is there): the deck's mean is
// the same near and far, and what the cache bakes at a cell's footprint is
// what the march's samples at a pixel's average to over that cell.

// How far the skin moves the surface at most, of the base, either way.
const float CLOUD_SKIN = 0.2;
// How much faster than 0 to 1 over `forms.edge` metres the world's base
// may rise at most up or down (cloudDensity's contract: it is the least of
// a field rising no faster than that any way and one rising no faster than
// this up or down and a fifth of it across, as under a flat base), and how
// much faster still the cache's cut of a share into clouds may
// (cloudCutBase: twice its noise read at a cell's footprint, which changes
// by under a half a cell, and 1.5 times the share).
const float CLOUD_EDGE_STEEPEST = 3.0;
const float CLOUD_CUT_STEEPEST = 2.0;

// Body point `p` as the forms' noise sees it: carried by the wind.
vec3 cloudDrift(vec3 p) { return p - clouds.wind.xyz * clouds.shell.w; }

// The detail's strength at `footprint` (none where it is finer than that).
float cloudDetailAt(float footprint) {
    float D = clouds.forms.z;
    return 1.0 - smoothstep(D / 64.0, D / 24.0, footprint);
}
// The base a footprint of `footprint` metres spans across a cloud's edge
// (how wide the surface's blur is, of the base; all of it from a footprint
// as wide as the edge).
float cloudBlur(float footprint) { return clamp(footprint / clouds.forms.w, 2e-3, 1.0); }
// And more where the skin varies within the footprint (the billows' noise
// `n` read over it: its spread, n.w less n.x squared), so a skin finer than
// a pixel is its mean there, a soft edge, never grain: as much as the skin
// moves the surface over that spread, at most CLOUD_SPREAD_MOST, from a
// footprint of a texel of the billows' noise (finer, a lookup is a point of
// it and spreads by none). cloudBlurMost the most it may be.
const float CLOUD_SPREAD_MOST = 0.12;
const float CLOUD_NOISE_PIXELS = 2.0;
float cloudSpreadAt(float footprint) {
    float S = clouds.forms.y / float(CL_NOISE_SIDE);
    return smoothstep(S, 4.0 * S, footprint);
}
float cloudBlurOf(vec4 n, float footprint) {
    float sd = sqrt(max(n.w - n.x * n.x, 0.0));
    return min(cloudBlur(footprint) + min(0.55 * sd, CLOUD_SPREAD_MOST) * cloudSpreadAt(footprint), 1.0);
}
float cloudBlurMost(float footprint) {
    return min(cloudBlur(footprint) + CLOUD_SPREAD_MOST * cloudSpreadAt(footprint), 1.0);
}
// The noises cloudForms reads at `pd`: the billows' and (where `detail`)
// the detail's, at their mean over CLOUD_NOISE_PIXELS footprints (a skin's
// lobe a pixel or two across, high in contrast at the surface, reads as
// grain: the skin shows what is a few pixels wide or more, a soft edge
// below). (Apart from the forms' arithmetic, so a caller that also
// reads the cache issues every lookup before it waits on any.)
vec4 cloudBillows(vec3 pd, float footprint) {
    float S = clouds.forms.y;
    return cloudNoise(pd / S, CLOUD_NOISE_PIXELS * footprint / S);
}
vec4 cloudDetail(vec3 pd, float footprint) {
    float D = clouds.forms.z;
    return cloudNoise(pd * vec3(1.0, 1.3, 1.0) / D, CLOUD_NOISE_PIXELS * footprint / D);
}
// The skin's move of the surface (of the base, -CLOUD_SKIN .. 0.55
// CLOUD_SKIN) from the noises `n` (cloudBillows) and `e` (cloudDetail, read
// where `wD`, cloudDetailAt, is above zero), `inCloud` the height within the
// cloud's column (0 its base .. 1 its top: the detail's wisps under,
// cauliflower over).
float cloudSkinOf(vec4 n, vec4 e, float wD, float inCloud) {
    float s = 0.55 * clamp(2.5 * (n.x - 0.5), -1.0, 1.0);
    if (wD > 0.0) {
        float fine = e.y * 0.6 + e.z * 0.4;
        s -= 0.45 * wD * mix(1.0 - fine, fine, smoothstep(0.0, 0.2, inCloud));
    }
    return CLOUD_SKIN * s;
}
// The forms' field at base `base` with the skin's move `skin`, blurred by
// `w` (cloudBlurOf): positive in cloud, its surface at -w/2, the share of
// the footprint the cloud fills 0.5 + field / w.
float cloudFieldOf(float base, float skin, float w) {
    return base + skin * (1.0 - w) - 0.5;
}
// How much the base read from a cell `cell` metres wide is the share of it
// the clouds fill rather than their surface (cloudDensity's contract: a
// cell as wide as a cloud's edge, about the least a cloud's size, is all
// share).
float cloudShareLike(float cell) { return smoothstep(0.25, 1.0, cell / clouds.forms.w); }
// The noise a share is cut into clouds by (cloudSurfaceBase): the engine's
// at CLOUD_CUT_TILE edges a tile, its billows some one and a half edges
// across, about the least a cloud's size, with the same turned and 0.62 as
// large (so neither's lattice lines the clouds up in rows), their sum
// spread back to one's.
const float CLOUD_CUT_TILE = 13.0;
const mat3 kCloudCutTurn = mat3(0.36, 0.48, -0.8, -0.8, 0.6, 0.0, 0.48, 0.64, 0.6);
vec4 cloudCutNoise(vec3 pd, float footprint) {
    float S = CLOUD_CUT_TILE * clouds.forms.w;
    vec4 a = cloudNoise(pd / S, footprint / S);
    vec4 b = cloudNoise(kCloudCutTurn * pd / (0.62 * S), footprint / (0.62 * S));
    return vec4(0.6 + (0.5 * (a.x + b.x) - 0.6) * 1.4, a.yzw);
}
// The base the cache keeps for a cell `cell` metres wide at `pd` (drifted)
// where the world's is `base`: as the world gives it where the cell is fine
// enough that it is the clouds' surface, and where it is wide enough that
// it is a share of the cell (cloudShareLike), that share cut into clouds by
// the noise (cloudCutNoise at the cell's footprint: cloud where it is over
// a threshold that falls as the share rises), so a far field the cache
// holds only as shares is still clouds and gaps, about as many as the
// share says, which the march reads as it reads the heaps (under 0 as far
// as the noise and the share take it: how far the clouds it cuts are).
float cloudCutBase(float base, vec3 pd, float cell) {
    float q = cloudShareLike(cell);
    if (q <= 0.0) return base;
    // (The noise as a score, about normal: its mean and spread. It is read
    // at a quarter of the cell, so a cell is a sample of it, not its mean,
    // and keeps its spread; its clouds a few cells across at the least.
    // The threshold the score's quantile for the share, logistic; under a
    // share of 2% steeply higher, out of the score's reach by none.)
    float z = (cloudCutNoise(pd, 0.25 * cell).x - 0.77) / 0.074;
    float s = clamp(base, 0.02, 0.98);
    float t = log((1.0 - s) / s) / 1.7 + max(0.02 - base, 0.0) * 150.0;
    float cut = min(0.5 + 0.25 * (z - t), 1.0);
    return mix(base, cut, q);
}
// The share of the footprint the cloud fills where the billows' noise is
// `n` (no detail: the layer's, the cache's), `base` as a surface.
float cloudFormsShare(vec4 n, float base, float footprint) {
    if (base <= 0.0) return 0.0;
    float w = cloudBlurOf(n, footprint);
    return clamp(0.5 + cloudFieldOf(base, cloudSkinOf(n, vec4(0.0), 0.0, 0.5), w) / w, 0.0, 1.0);
}
// The extinction (1/m) of base `base` with the skin's move `skin` over
// `footprint`, blurred by `w`: a cloud's heart's within (thinner within the skin, so its
// edge is soft and lit through), and from afar what a footprint's clouds
// leave of the light over a path (opaque at heart where they are, all of it
// between them) as an extinction, so a field from orbit lets the sky
// between its clouds through. (The path a cloud's own: a footprint, or a
// cloud's edge where that is shorter, the least a cloud's size; a view
// crosses one cloud or the gap beside it in that length, whatever the
// footprint, so a field reads as opaque from afar as its clouds cover.)
float cloudSigmaOf(float base, float skin, float w, float footprint) {
    if (base <= 0.0) return 0.0;
    float v = cloudFieldOf(base, skin, w);
    float share = clamp(0.5 + v / w, 0.0, 1.0);
    if (share <= 0.0) return 0.0;
    float heart = clouds.forms.x;
    float near = heart * share * mix(0.35, 1.0, smoothstep(0.0, CLOUD_SKIN, v));
    float L = clamp(footprint, 20.0, clouds.forms.w);
    float through = 1.0 - share * (1.0 - exp(-heart * L));
    return mix(near, -log(max(through, 1e-4)) / L, w);
}
float cloudFormsOf(vec4 n, vec4 e, float wD, float base, float inCloud, float footprint) {
    return cloudSigmaOf(base, cloudSkinOf(n, e, wD, inCloud), cloudBlurOf(n, footprint), footprint);
}
float cloudForms(vec3 pd, float base, float inCloud, float footprint, bool detail) {
    if (base <= 0.0) return 0.0;
    float wD = detail ? cloudDetailAt(footprint) : 0.0;
    vec4 e = wD > 0.0 ? cloudDetail(pd, footprint) : vec4(0.0);
    return cloudFormsOf(cloudBillows(pd, footprint), e, wD, base, inCloud, footprint);
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

// --- The cache about the eye (sky/clouds_layout.h) ------------------------------

// Body direction `up` on the cache's chart (m).
vec2 cloudChartAt(vec3 up) {
    vec2 v = vec2(dot(up, clouds.cacheE1.xyz), dot(up, clouds.cacheE2.xyz));
    float s = length(v);
    float th = atan(s, dot(up, clouds.cacheC.xyz));
    return s > 1e-7 ? v * (th * clouds.cacheC.w / s) : v * clouds.cacheC.w;
}

// The chart's point `xy` (m) as a body direction.
vec3 cloudChartDir(vec2 xy) {
    float d = length(xy);
    float th = d / clouds.cacheC.w;
    vec2 b = d > 0.0 ? xy / d : vec2(1.0, 0.0);
    return normalize(clouds.cacheC.xyz * cos(th) + (clouds.cacheE1.xyz * b.x + clouds.cacheE2.xyz * b.y) * sin(th));
}

float cloudCacheCell(int k) { return CL_CACHE_TEXEL0 * exp2(float(k)); }

// Storage index `i` of the window about cell `c`: the cell it holds. (The
// side a power of two: the mask is the modulus of a negative difference
// too, as GLSL's % is not.)
int cloudCacheCellOf(int i, int c) {
    int lo = c - CL_CACHE_SIDE / 2;
    return lo + ((i - lo) & (CL_CACHE_SIDE - 1));
}

// The finest level whose cell is at least `foot` and whose window holds
// chart point `xy` (CL_CACHE_LEVELS: none does).
int cloudCacheLevel(vec2 xy, float foot) {
    vec2 dn = abs(xy - clouds.cacheNadir.xy);
    float far = max(dn.x, dn.y) / (CL_CACHE_MARGIN * CL_CACHE_TEXEL0);
    float k = ceil(log2(max(max(foot / CL_CACHE_TEXEL0, far), 1.0)));
    return int(min(k, float(CL_CACHE_LEVELS)));
}

// Where chart point `xy` at height `h` in the deck (0 .. 1) lies in the
// cache's image at level `k`.
vec3 cloudCacheCoord(vec2 xy, float h, int k) {
    const float B = float(CL_CACHE_BINS);
    vec2 uv = xy / (cloudCacheCell(k) * float(CL_CACHE_SIDE));
    float z = float(k) * B + clamp(h * B, 0.5, B - 0.5);
    return vec3(uv, z / (B * float(CL_CACHE_LEVELS)));
}

// The band of the columns about chart point `xy` at level `k`.
vec4 cloudCacheBand(vec2 xy, int k) {
    return textureLod(cloudBandMap, vec3(xy / (cloudCacheCell(k) * float(CL_CACHE_SIDE)), float(k)), 0.0);
}
// The column at chart point `xy` as a layer, at level `k` (sky/clouds_layout.h's
// CL_B_BAND): x its optical depth up the deck, y the mean depth from its top
// of what it sends up, z the mean height (0 .. 1) of that.
vec4 cloudCacheColumn(vec2 xy, int k) {
    return textureLod(cloudBandMap, vec3(xy / (cloudCacheCell(k) * float(CL_CACHE_SIDE)), float(CL_CACHE_LEVELS + k)),
                      0.0);
}
