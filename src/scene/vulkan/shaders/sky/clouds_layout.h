// The cloud deck's shared constants (scene/world/clouds.h, vulkan/sky/
// scene_clouds.h): for C++ and GLSL alike.
//
//   The clouds' set (one layout for the march, the cover's bake and the
//   shadow's levels; what a shader does not read is bound to a stand-in):
//     CL_B_UBO        the clouds' block (CloudsGpu, sky/scene_cloud_layer.glsl)
//     CL_B_COVER      the cover, sampled: a cube of CL_COVER_SIDE texels a
//                     face (its border included), a face an array layer
//     CL_B_NOISE      the engine's tiling noise, sampled (CL_NOISE_SIDE^3,
//                     its mips down to a texel)
//     CL_B_DEPTH      the opaque depth (SceneTargets::depthSnapshot): the march
//     CL_B_COVER_OUT  the cover, written by the bake (rgba16f storage)
//     CL_B_SHADOW_OUT the shadow's levels, written (r32f storage, a level a layer)
//     CL_B_LAND_GRID  the land's processes' grid (the bake's cloudLandGrid)
//     CL_B_CACHE      the deck's cache about the eye, sampled (below): its
//                     base and the depths toward the sun and up
//     CL_B_CACHE_OUT  the same, written by its light's bake (rgba16f storage)
//     CL_B_SIGMA      the cache's extinction (x, the forms' mean over a
//                     cell) and base (y), sampled (the light's bake, the shadow)
//     CL_B_SIGMA_OUT  the same, written by the module's bake (rg16f storage)
//     CL_B_BAND       the cache's columns' bands, sampled: a level a layer,
//                     x the lowest height (0 .. 1 up the deck) that holds
//                     cloud about the column (its own and its neighbours'),
//                     y the highest, z the most a column about it holds
//                     (its optical depth up the whole deck), w its own;
//                     and from layer CL_CACHE_LEVELS on, the column as a
//                     layer at each level (cloudCacheColumn)
//     CL_B_BAND_OUT   the same, written by the light's bake (rgba16f storage)
//     CL_B_DIST_A/B   the distance transform's scratch (r16f storage, as the
//                     cache: sky/clouds_dist.comp)
//
//   The lighting set's binding 24: the shadow's levels, sampled
//   (CloudShadowGpu in the lighting block says where they lie).

#ifndef BRO_SCENE_CLOUDS_LAYOUT_H
#define BRO_SCENE_CLOUDS_LAYOUT_H

#define CL_B_UBO            0
#define CL_B_COVER          1
#define CL_B_NOISE          2
#define CL_B_DEPTH          3
#define CL_B_COVER_OUT      4
#define CL_B_SHADOW_OUT     5
#define CL_B_LAND_GRID      6
#define CL_B_CACHE          7
#define CL_B_CACHE_OUT      8
#define CL_B_SIGMA          9
#define CL_B_SIGMA_OUT      10
#define CL_B_BAND           11
#define CL_B_BAND_OUT       12
#define CL_B_DIST_A         13
#define CL_B_DIST_B         14

#define CL_MAX_DATA         64       // cloudData(i): vec4s

// The cover: a cube about the body, CL_COVER_SIDE texels a face, the first
// and last row and column a texel past the face's edge (the neighbouring
// face's ground), so a lookup bilinear across an edge is seamless.
#define CL_COVER_SIDE       1024
#define CL_COVER_GROUP      8

// The engine's noise: CL_NOISE_SIDE^3 texels, tiling, its mips to one texel.
#define CL_NOISE_SIDE       128
#define CL_NOISE_MIPS       8
#define CL_NOISE_GROUP      4

// The clouds' shadow on the land: CL_SHADOW_LEVELS square levels of
// CL_SHADOW_SIDE texels on the plane tangent to the body under the eye,
// level l's texel CL_SHADOW_TEXEL0 * CL_SHADOW_GROWTH^l metres (on that
// plane), each texel what the deck leaves of the sun at the datum there.
#define CL_SHADOW_LEVELS    7
#define CL_SHADOW_SIDE      256
#define CL_SHADOW_TEXEL0    60.0
#define CL_SHADOW_GROWTH    4.0
#define CL_SHADOW_STEPS     12
#define CL_SHADOW_GROUP     8

// The deck's cache about the eye: what the march reads instead of the
// world's module wherever a cell is as fine as a pixel (and everywhere for
// its gaps and its light). A chart fixed to the body, about a point near
// the eye's nadir (azimuthal equidistant: a direction's coordinates its
// distance from that point along the deck's bottom, in metres, toward its
// bearing), and on it CL_CACHE_LEVELS square windows of CL_CACHE_SIDE
// cells about the nadir, level k's cell CL_CACHE_TEXEL0 * 2^k metres, each
// column of a cell CL_CACHE_BINS bins up the deck. A cell is stored at its
// index modulo the side (toroidally: a window that moves keeps what it
// still covers, and a lookup wraps); the levels lie one above another in z
// of one 3D image, CL_CACHE_BINS each. A texel of the cache: x the world's
// base there (its cloudDensity at the cell's footprint, which the march
// cuts into the engine's forms), y the optical depth from there toward the
// sun out of the deck (through the forms' mean extinction), z the same
// straight up, w how far (m) the nearest cloud is, out to CL_CACHE_REACH
// cells across and the column up (sky/clouds_dist.comp). A level is baked
// whole when its window moves, every level when the deck, the chart or the
// time jumps, and otherwise every CL_CACHE_SLICES-th row of each a frame (the wind, the
// sun's motion).
#define CL_CACHE_LEVELS     13
#define CL_CACHE_SIDE       128
#define CL_CACHE_BINS       32
#define CL_CACHE_TEXEL0     62.5
#define CL_CACHE_MARGIN     62.0     // cells about the nadir a level's lookups stay within
#define CL_CACHE_SLICES     16
#define CL_CACHE_REACH      8        // cells across the distance to cloud is sought over
#define CL_CACHE_GROUP      8

#endif
