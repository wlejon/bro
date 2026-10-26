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
//   bool cloudHeap(CloudHeapCell c, out CloudHeap h)     (with #define CLOUD_HEAPS)
//       the heap cell `c` of the engine's lattice holds (sky/cloud_heaps.glsl,
//       setClouds' heaps.cell metres a cell), its lobes within a cell of
//       its middle; false none. The engine draws the heaps as themselves
//       at every distance (the heaps' map, sky/clouds_layout.h), so
//       cloudDensity gives what is not heaps: slabs.
//   float cloudHeapsShare(CloudPoint q)                  (with CLOUD_HEAPS)
//       the share of the air the heaps fill at q, on the mean over
//       q.footprint (read where they are finer than it), above 0 however
//       little wherever a heap may stand and under 0 where none can: the
//       march's bounds trust it.
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
    vec4 wind;          // xyz the wind (m/s, the body's frame): what the forms drift with; w the heaps' cell (m, 0 none)
    vec4 heapNadir;     // xy the drifted nadir on the cache's chart (m), z 1 when the heaps' map is baked
    vec4 heapLevel[CL_HEAP_LEVELS];   // xy the texel (index) of level k of the heaps' map that holds it
    vec4 data[CL_MAX_DATA];
    // The volume about the eye (sky/cloud_volume.glsl).
    vec4 volA;          // xyz its anchor (the drifted body frame, m), w the finest cell (m)
    vec4 volE1;         // xyz its frame's first axis, w the slack (m) its distances are read less
    vec4 volE2;         // xyz the second, w the debug view (CL_DEBUG_VOLUME)
    vec4 volN;          // xyz up at the anchor, w the debug slice's height (m over the deck's bottom)
    vec4 volEye;        // xyz the eye in the volume's frame (m), w a pixel's angle (radians)
    vec4 volLevel[CL_VOL_LEVELS];   // xyz the cell (index) level k's window is about, w 1 once baked
} clouds;

layout(set = CLOUD_SET, binding = CL_B_COVER) uniform sampler2DArray cloudCoverMap;
layout(set = CLOUD_SET, binding = CL_B_NOISE) uniform sampler3D cloudNoiseMap;
layout(set = CLOUD_SET, binding = CL_B_CACHE) uniform sampler3D cloudCacheMap;
layout(set = CLOUD_SET, binding = CL_B_SIGMA) uniform sampler3D cloudSigmaMap;
layout(set = CLOUD_SET, binding = CL_B_BAND) uniform sampler2DArray cloudBandMap;
layout(set = CLOUD_SET, binding = CL_B_GLOBE) uniform sampler2DArray cloudGlobeMap;
layout(set = CLOUD_SET, binding = CL_B_HEAPS) uniform sampler2DArray cloudHeapMap;

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

// A heap (sky/cloud_heaps.glsl): lobes, half-ellipsoids each standing on
// its base: the ellipsoid's middle at xy metres off the cell's middle on
// the plane tangent there (east, north) and `middle` metres over its base
// (under 0: below it), z across and w up its half-axes, cut flat at `base`
// (metres up the deck). w 0: no lobe.
const int CL_HEAP_LOBES = 4;
struct CloudHeap {
    vec4 lobe[CL_HEAP_LOBES];
    vec4 middle;
    vec4 base;
};
// A cell of the heaps' lattice, as the world is asked about it.
struct CloudHeapCell {
    ivec3 id;      // x the cube's face, y and z the cell on it
    vec3 up;       // its middle's direction (the drifted body frame)
    vec3 east;     // the plane tangent there (unit)
    vec3 north;
    vec3 hash;     // three uniform draws of its own, 0 .. 1
    vec4 cover;    // cloudCoverAt(up)
    float side;    // its width (m, at the deck's bottom)
};

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

// Where body direction `up` lies in a cube of `S` texels a face with a
// border (the cover's, the globe's): xy its texture coordinates, z its
// face. (The face's axes by branch, not from the tables: a table indexed
// by a value the shader works out is read from memory on some GPUs, and
// this runs for every sample.)
vec3 cloudCubeCoord(vec3 up, float S) {
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
    return vec3(((e * 0.5 + 0.5) * (S - 2.0) + 1.0) / S, float(f));
}
// The cover at body direction `up`, bilinear.
vec4 cloudCoverAt(vec3 up) {
    return textureLod(cloudCoverMap, cloudCubeCoord(up, float(CL_COVER_SIDE)), 0.0);
}
// The globe's column at body direction `up` (sky/clouds_layout.h's
// CL_GLOBE_SIDE), bilinear, as cloudCacheColumn's; and its texel's width
// (m, at a face's middle).
vec4 cloudGlobeColumn(vec3 up) {
    return textureLod(cloudGlobeMap, cloudCubeCoord(up, float(CL_GLOBE_SIDE)), 0.0);
}
float cloudGlobeTexel() { return clouds.shell.x * 2.0 * CLOUD_QUARTER_PI / float(CL_GLOBE_SIDE - 2); }

// A column as a layer seen from above (the cache's, sky/clouds_light.comp,
// and the globe's, sky/clouds_globe.comp), its bins added top down: `d`
// a bin's optical depth up it, `h` its height in the deck (0 .. 1). What
// each bin sends weighs as much as it takes of the view; the layer's depth
// is the clouds' cover over the column, not the bins' sum: a cloud stands
// over many bins (they overlap most), so a field of heaps whose share of
// each height is a third covers a third of the view from above however
// deep it is, a veil as grey as they cover. A bin's cover is the share an
// edge of it takes (cloudSigmaOf's clumped extinction over the edge: the
// share where the clouds are opaque over an edge); the column's the most
// of its bins', the bins' sum over it the depth within its clouds.
// (Where the bins are shares of clouds wider than they are, `q` of it,
// cloudShareLike: their extinction is a share's over a cloud's edge, the
// path a view crosses one cloud or the gap beside it in, cloudSigmaOf; but
// a bin is far less deep than an edge, and straight down a view crosses
// the whole of the cloud in it, a heart's extinction over the bin. So the
// depth within the clouds is that, by the share: a deck of opaque clouds
// over a share of the column is that share of the view, opaque, not a thin
// veil over all of it, which lit as one sent back a grey wash over the
// sea.)
struct CloudColumn {
    float tau, took, depth, height, cover, inner;
};
CloudColumn cloudColumnNone() { return CloudColumn(0.0, 0.0, 0.0, 0.0, 0.0, 0.0); }
void cloudColumnAdd(inout CloudColumn c, float d, float h, float binH, float q) {
    if (d <= 0.0) return;
    float wgt = exp(-c.tau) * (1.0 - exp(-d));
    c.took += wgt;
    c.depth += wgt * (c.tau + 0.5 * min(d, 1.0));
    c.height += wgt * h;
    c.tau += d;
    float share = 1.0 - exp(-d / binH * clouds.forms.w);
    c.cover = max(c.cover, share);
    c.inner += mix(d, max(d, share * clouds.forms.x * binH), q);
}
// The layer: x its optical depth (the view's, straight down), y the mean
// depth from its top of what it sends, z the mean height of that, w the
// depth within its clouds (what the light one of them sends has scattered
// about in: the powder's).
vec4 cloudColumnLayer(CloudColumn c) {
    float cover = max(c.cover, 1e-4);
    float inner = min(c.inner / cover, 1e3);
    float tau = -log(max(1.0 - cover * (1.0 - exp(-inner)), 0.005));
    return c.took > 0.0 ? vec4(tau, c.depth / c.took, c.height / c.took, inner) : vec4(0.0, 0.0, 0.5, 0.0);
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
// this up or down and a fifth of an edge's rate across, as under a flat
// base, where the march shows none of the skin's move: clouds_light.comp).
const float CLOUD_EDGE_STEEPEST = 3.0;

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
// The noises cloudForms reads at `pd`: the billows' and (where `detail`)
// the detail's, at their mean over the footprint (looked up a mip coarser,
// CLOUD_NOISE_PIXELS footprints: the skin's lobes a pixel or two across
// show little of their move anyway, cloudSkinAt, and the coarser mip's
// lookups are cheaper). (Apart from the forms' arithmetic,
// so a caller that also reads the cache issues every lookup before it
// waits on any.)
const float CLOUD_NOISE_PIXELS = 2.0;
vec4 cloudBillows(vec3 pd, float footprint) {
    float S = clouds.forms.y;
    return cloudNoise(pd / S, CLOUD_NOISE_PIXELS * footprint / S);
}
vec4 cloudDetail(vec3 pd, float footprint) {
    float D = clouds.forms.z;
    return cloudNoise(pd * vec3(1.0, 1.3, 1.0) / D, CLOUD_NOISE_PIXELS * footprint / D);
}
// How much of the skin's move shows over `footprint`: its lobes' least
// wavelength (CLOUD_LOBES of the billows' tile) as CLOUD_LOBE_PIXELS
// footprints or more, all of it; at half that, none. A lobe a pixel or two
// across is high in contrast at the surface and reads as grain, so a heap
// seen small is the world's smooth dome (lit, its lee side shaded), never
// noise.
const float CLOUD_LOBES = 16.0;
const float CLOUD_LOBE_PIXELS = 4.0;
float cloudSkinAt(float footprint) {
    float lobe = clouds.forms.y / CLOUD_LOBES;
    return smoothstep(0.5 * CLOUD_LOBE_PIXELS, CLOUD_LOBE_PIXELS, lobe / max(footprint, 1e-3));
}
// The surface's blur over `footprint` (of the base): the footprint's span
// across an edge (cloudBlur), and where the skin's move fades out, as much
// of CLOUD_SKIN_BLUR as it no longer shows: the skin's lobes under a few
// pixels are their mean there, a soft edge to the dome (and a surface that
// soft is quickly found). cloudSurfaceBlurMost the most it may be.
const float CLOUD_SKIN_BLUR = 0.12;
float cloudSurfaceBlur(float footprint) {
    return min(cloudBlur(footprint) + CLOUD_SKIN_BLUR * (1.0 - cloudSkinAt(footprint)), 1.0);
}
float cloudSurfaceBlurMost(float footprint) {
    return min(cloudBlur(footprint) + CLOUD_SKIN_BLUR, 1.0);
}
// A turret of the cauliflower from Worley's 1 - F1 `v` (the noise's y or
// z): a dome over its cell, 1 at the cell's heart falling ever faster to 0
// at its rim, so neighbouring turrets meet in a sharp crease, not a soft
// trough (1 - F1 itself is a cone; its mean as a surface is soft bumps).
// Its slope (of `v`) as `dv`: the dome is a circle's eased by
// CLOUD_TURRET_EASE, so it meets its rim at a slope of about eight, not
// upright (an upright wall moves the surface by a whole turret within a
// pixel, however fine, and its silhouette steps from pixel to pixel).
const float CLOUD_TURRET_EASE = 0.05;
float cloudTurret(float v, out float dv) {
    float f = (1.0 - v) * 1.45;
    float r0 = sqrt(CLOUD_TURRET_EASE);
    float r = sqrt(max(1.0 - f * f, 0.0) + CLOUD_TURRET_EASE);
    float k = 1.0 / (sqrt(1.0 + CLOUD_TURRET_EASE) - r0);
    dv = f < 1.0 ? k * 1.45 * max(f, 0.0) / r : 0.0;
    return (r - r0) * k;
}
// How much Worley's 1 - F1 (two finer octaves, 0.625 + 0.25 + 0.125)
// changes at most a metre, for cells `cell` metres wide.
float cloudWorleySlope(float cell) { return 1.625 / cell; }
// The skin's move of the surface (of the base, -CLOUD_SKIN .. 0.55
// CLOUD_SKIN, `amp` of it: cloudSkinAt and how much the world lets it
// move there) from the noises `n` (cloudBillows) and `e` (cloudDetail, read
// where `wD`, cloudDetailAt, is above zero), `inCloud` the height within the
// cloud's column (0 its base .. 1 its top: the detail's wisps under,
// cauliflower over). The cauliflower is turrets on three scales, each a
// dome on Worley's cells (the billows' cells, a quarter of their tile, and
// the detail's at a quarter and an eighth of its), the larger ones
// carrying the smaller, deep creases between them where the sun does not
// reach; the billows' Perlin-Worley only the walls' slow heave. The most
// the turrets move it a metre (of the base) as `slope`: the surface's blur
// takes it in, so where they make the surface steeper than the base, its
// edge still ramps over the footprint (cloudSkinBlur).
float cloudSkinOf(vec4 n, vec4 e, float wD, float inCloud, float amp, out float slope) {
    float s = 0.3 * clamp(2.5 * (n.x - 0.5), -1.0, 1.0);
    float dy, dz, dn;
    float c = 0.5 * cloudTurret(n.y, dn), cw = 0.5;
    float dc = 0.5 * dn * cloudWorleySlope(0.25 * clouds.forms.y);
    float under = 0.0;
    if (wD > 0.0) {
        c += wD * (0.3 * cloudTurret(e.y, dy) + 0.2 * cloudTurret(e.z, dz));
        dc += wD * (0.3 * dy * cloudWorleySlope(0.25 * clouds.forms.z) +
                    0.2 * dz * cloudWorleySlope(0.125 * clouds.forms.z));
        cw += 0.5 * wD;
        under = -0.45 * wD * (1.0 - (e.y * 0.6 + e.z * 0.4));
    }
    // (The turrets from just over the base up, their creases cut in from
    // there but their domes pushed out only higher: low on the wall none
    // hangs over the flat base like a drip, and the wall is never a smooth
    // rounded band under them.)
    float top = 0.5 * smoothstep(0.1, 0.4, inCloud);
    float cauli = mix(-0.85, top, c / cw);
    float up = smoothstep(0.03, 0.2, inCloud);
    s += mix(under, cauli, up);
    slope = CLOUD_SKIN * amp * up * (top + 0.85) * dc / cw;
    return CLOUD_SKIN * clamp(s, -1.0, 0.55) * amp;
}
float cloudSkinOf(vec4 n, vec4 e, float wD, float inCloud, float amp) {
    float slope;
    return cloudSkinOf(n, e, wD, inCloud, amp, slope);
}
// The blur the skin's `slope` (cloudSkinOf) adds over `footprint`: the
// surface's move across it.
float cloudSkinBlur(float slope, float footprint) { return slope * footprint; }
// The forms' field at base `base` with the skin's move `skin`, blurred by
// `w` (cloudBlur): positive in cloud, its surface at -w/2, the share of
// the footprint the cloud fills 0.5 + field / w.
float cloudFieldOf(float base, float skin, float w) {
    return base + skin * (1.0 - w) - 0.5;
}
// How much the base read from a cell `cell` metres wide is the share of it
// the clouds fill rather than their surface (cloudDensity's contract: a
// cell as wide as a cloud's edge, about the least a cloud's size, is all
// share).
float cloudShareLike(float cell) { return smoothstep(0.25, 1.0, cell / clouds.forms.w); }

// The share of the footprint the cloud fills where the billows' noise is
// `n` (no detail: the layer's, the cache's), `base` as a surface.
float cloudFormsShare(vec4 n, float base, float footprint) {
    if (base <= 0.0) return 0.0;
    float w = cloudSurfaceBlur(footprint);
    float skin = cloudSkinOf(n, vec4(0.0), 0.0, 0.5, cloudSkinAt(footprint));
    return clamp(0.5 + cloudFieldOf(base, skin, w) / w, 0.0, 1.0);
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
// (`ramp`, w or more, how wide the share's ramp is across the surface, in
// from its outer edge at -w/2: the field's span over the footprint where
// the skin is steeper than the base. In, so the surface reaches out no
// further than w says, as the march's bounds take it: clBaseClear.)
float cloudSigmaOf(float base, float skin, float w, float footprint, float ramp) {
    if (base <= 0.0) return 0.0;
    float v = cloudFieldOf(base, skin, w);
    float share = clamp((v + 0.5 * w) / ramp, 0.0, 1.0);
    if (share <= 0.0) return 0.0;
    float heart = clouds.forms.x;
    float near = heart * share * mix(0.35, 1.0, smoothstep(0.0, CLOUD_SKIN, v));
    float L = clamp(footprint, 20.0, clouds.forms.w);
    float through = 1.0 - share * (1.0 - exp(-heart * L));
    return mix(near, -log(max(through, 1e-4)) / L, w);
}
float cloudSigmaOf(float base, float skin, float w, float footprint) {
    return cloudSigmaOf(base, skin, w, footprint, w);
}
float cloudFormsOf(vec4 n, vec4 e, float wD, float base, float inCloud, float footprint) {
    return cloudSigmaOf(base, cloudSkinOf(n, e, wD, inCloud, cloudSkinAt(footprint)), cloudSurfaceBlur(footprint), footprint);
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
// The same of its slabs alone (the heaps left out).
vec4 cloudCacheSlabColumn(vec2 xy, int k) {
    return textureLod(cloudBandMap,
                      vec3(xy / (cloudCacheCell(k) * float(CL_CACHE_SIDE)), float(2 * CL_CACHE_LEVELS + k)), 0.0);
}

// --- The heaps' map about the eye (sky/clouds_layout.h) --------------------------

float cloudHeapTexel(int k) { return CL_HEAP_TEXEL0 * exp2(float(k)); }
// The heaps' cell (m) where the map holds them (0: none).
float cloudHeapMapSide() { return clouds.heapNadir.z > 0.5 ? clouds.wind.w : 0.0; }

// The map's level about footprint `foot` at chart point `xy` (a drifted
// point's), as a fraction, as cloudCacheLod's.
float cloudHeapLod(vec2 xy, float foot) {
    vec2 dn = abs(xy - clouds.heapNadir.xy);
    float far = max(dn.x, dn.y) / (CL_HEAP_MARGIN * CL_HEAP_TEXEL0);
    return max(max(log2(max(foot, 1e-3) / CL_HEAP_TEXEL0), log2(max(far, 1e-6)) + 1.0), 0.0);
}

// The heaps' column (sky/cloud_heaps.glsl's cloudHeapColumn) at the
// drifted point's chart point `xy` over footprint `foot`, blended over the
// two levels about it; and the texel it was read over (y; past the map, or
// with no map, no heaps: z -1e5).
vec4 cloudHeapMapColumn(vec2 xy, float foot, out float texel) {
    float lod = cloudHeapLod(xy, foot);
    int k = int(lod);
    texel = cloudHeapTexel(min(k, CL_HEAP_LEVELS - 1)) * exp2(fract(lod));
    if (clouds.heapNadir.z < 0.5 || k >= CL_HEAP_LEVELS) return vec4(0.0, 0.0, -1e5, 1.0);
    const float S = float(CL_HEAP_SIDE);
    vec4 a = textureLod(cloudHeapMap, vec3(xy / (cloudHeapTexel(k) * S), float(k)), 0.0);
    float f = fract(lod);
    if (k + 1 < CL_HEAP_LEVELS && f > 0.0)
        a = mix(a, textureLod(cloudHeapMap, vec3(xy / (cloudHeapTexel(k + 1) * S), float(k + 1)), 0.0), f);
    return a;
}
// How far (m) a point `z` metres up the deck is inside the heaps of column
// `c` at most (negative: outside, by at least about that): under the top
// and over the bottom (as far as under the top a metre), inside the outline
// across.
float cloudHeapColumnIn(vec4 c, float z) {
    float zr = z - c.x;
    return min(min(zr, c.y - zr) * c.w, c.z);
}
// How much of the heaps a footprint `foot` (or the heaps' map's texel
// `texel` read for it) sees as their share, not as themselves: where they
// are a few pixels across or less, their share (the cache's), so a field of
// them far off is the veil its heaps make; and all of it by the map's
// coarsest texels, so past the map nothing changes.
float cloudHeapsFar(float foot, float texel) {
    float hs = cloudHeapMapSide();
    return max(smoothstep(0.25 * hs, 0.6 * hs, foot), smoothstep(0.4 * hs, 0.8 * hs, texel));
}

// --- The layer seen from above (clouds.frag) -------------------------------------

const float CL_LAYER_FROM = 0.025; // a pixel's footprint (of the deck's thickness) where the layer begins
const float CL_LAYER_TO = 0.05;    // and where it is all
const float CL_GLOBE_FROM = 1.0;   // the cache's cell (of the globe's texel) where the layer begins to read the globe
const float CL_GLOBE_TO = 2.0;     // and where it reads the globe alone
const float CL_GLOBE_CLOUDS_FROM = 0.3;   // a pixel's footprint (edges) where the layer begins to read the globe
const float CL_GLOBE_CLOUDS_TO = 1.0;     // and where it reads it alone

// The cache's level about footprint `foot` at chart point `xy`, as a
// fraction: its whole part the finer of the two read (whose window holds
// the point), the rest how far toward the coarser.
float cloudCacheLod(vec2 xy, float foot) {
    vec2 dn = abs(xy - clouds.cacheNadir.xy);
    float far = max(dn.x, dn.y) / (CL_CACHE_MARGIN * CL_CACHE_TEXEL0);
    return max(max(log2(max(foot, 1e-3) / CL_CACHE_TEXEL0), log2(max(far, 1e-6)) + 1.0), 0.0);
}
// The cell (m) the layer's column is read over at chart point `xy` for a
// pixel of footprint `foot` (at the layer's level `lod`, cloudCacheLod):
// the cache's there, toward the globe's as clouds.frag blends to it, and
// how much it reads the globe (y).
vec2 cloudLayerCell(float lod, float foot) {
    int k = int(lod);
    float cell = cloudCacheCell(min(k, CL_CACHE_LEVELS - 1)) * exp2(k < CL_CACHE_LEVELS ? fract(lod) : 1.0);
    float globe = max(smoothstep(CL_GLOBE_FROM, CL_GLOBE_TO, cell / cloudGlobeTexel()),
                      smoothstep(CL_GLOBE_CLOUDS_FROM, CL_GLOBE_CLOUDS_TO, foot / clouds.forms.w));
    return vec2(mix(cell, cloudGlobeTexel(), globe), globe);
}
