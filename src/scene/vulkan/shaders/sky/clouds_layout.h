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
//     CL_B_CACHE      the deck's cache about the eye, sampled (below): the
//                     slabs' alone depth toward the sun (x: in the bottom
//                     bin, the land's shadow's; above, y), the depths
//                     toward the sun and up, the free distance (w)
//     CL_B_CACHE_OUT  the same, written by its light's bake (rgba16f storage)
//     CL_B_SIGMA      the cache's extinction (x, the forms' mean over a
//                     cell), base (y) and how much of the skin's move shows
//                     (z: none at a flat base), sampled (the march, the
//                     light's bake, the shadow)
//     CL_B_SIGMA_OUT  the same, written by the module's bake (rgba16f storage)
//     CL_B_BAND       the cache's columns' bands, sampled: a level a layer,
//                     x the lowest height (0 .. 1 up the deck) that may hold
//                     cloud about the column (its own and its neighbours':
//                     CL_B_HOLDS), y the highest, z the most a column about
//                     it holds (its optical depth up the whole deck; none
//                     only where none may hold any);
//                     and from layer CL_CACHE_LEVELS on, the column as a
//                     layer at each level (cloudCacheColumn), and from
//                     2 CL_CACHE_LEVELS on its slabs' alone
//                     (cloudCacheSlabColumn)
//     CL_B_BAND_OUT   the same, written by the light's bake (rgba16f storage)
//     CL_B_DIST_A/B   the distance transform's scratch (r16f storage, as the
//                     cache: sky/clouds_dist.comp)
//     CL_B_HOLDS     whether each texel's cell of the cache may hold cloud
//                     as the march sees it, however near (r16f storage, as
//                     the cache: sky/clouds_cache.comp; the band and the
//                     distance to cloud are made from it)
//     CL_B_GLOBE      the globe's columns, sampled: a cube about the body
//                     as the cover's (below), each texel its column as a
//                     layer (sky/clouds_globe.comp)
//     CL_B_GLOBE_OUT  the same, written by its bake (rgba16f storage)
//     CL_B_VOLUME     the volume about the eye, sampled: CL_VOL_LEVELS 3D
//                     images, a level each (below; sky/cloud_volume.glsl)
//     CL_B_VOLUME_OUT the same, written by its bakes (rgba16f storage)
//     CL_B_VOLUME_DA/DB its distance transform's scratch (r16f storage, a
//                     level each: sky/clouds_volume_dist.comp)
//     CL_B_VOLUME_CHECK the volume's check's results (a storage buffer:
//                     sky/clouds_volume_check.comp)
//     CL_B_HEAPS      the heaps' map about the eye, sampled (below:
//                     sky/clouds_heaps.comp), a level a layer
//     CL_B_HEAPS_OUT  the same, written by its bake (rgba16f storage)
//     CL_B_VIEW       the view's columns, sampled (below: sky/clouds_view.comp)
//     CL_B_VIEW_OUT   the same, written by its bake (rgba16f storage)
//     CL_B_SHELLS     the heaps drawn as shells past the march (a storage
//                     buffer, below: sky/clouds_shells.comp)
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
#define CL_B_HOLDS          15
#define CL_B_GLOBE          16
#define CL_B_GLOBE_OUT      17
#define CL_B_VOLUME         18
#define CL_B_VOLUME_OUT     19
#define CL_B_VOLUME_DA      20
#define CL_B_VOLUME_DB      21
#define CL_B_VOLUME_CHECK   22
#define CL_B_HEAPS          23
#define CL_B_HEAPS_OUT      24
#define CL_B_VIEW           25
#define CL_B_VIEW_OUT       26
#define CL_B_SHELLS         27

// The view's columns (sky/clouds_view.comp): where the layer draws the deck
// from above, a pixel is far finer than the cache's cell there (its windows
// lie about the nadir, 128 cells across, so from a few hundred kilometres
// up the view's far side reads cells of 16 km) or the globe's texel (some
// 10 km); each texel of an image over the view (CL_VIEW_W x CL_VIEW_H,
// `viewSize.w` pixels a texel each way, 2 or more) is the column where its
// ray enters the deck as a layer, the world's clouds at its own footprint
// over CL_VIEW_BINS heights, made each frame the eye is over the deck.
#define CL_VIEW_W           1920
#define CL_VIEW_H           1080
#define CL_VIEW_BINS        8
#define CL_VIEW_GROUP       8
#define CL_VIEW_HEAP_STEPS  4        // the heaps' share's heights a bin
#define CL_VIEW_HEAPS_AT    800.0    // m up the deck where a texel reads the heaps as themselves
#define CL_VIEW_HEAPS_LEAST 300.0    // m: a heap's rim's least height there
// A debug switch: -1 as drawn; 0 the heaps everywhere their share, 1 every
// texel the heaps as themselves, so a view's two looks can be compared on
// one frame (their means should agree: the share is the heaps' mean).
#define CL_VIEW_FORCE       -1
// How far in from a heap's outline its billowed surface lies on the mean
// (m): the march draws the billows' skin, mostly in from the outline, and
// the view's heaps as themselves must cover what the march's cover. (Their
// edge eased over two texels, a heap a few pixels across a soft puff, not a
// hard white point; the inset is where that edge's middle covers what the
// share does, 250 m with an edge one texel wide.)
#define CL_VIEW_HEAP_INSET  650.0

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
// plane), each texel what the deck leaves of the sun at the datum there:
// sky/clouds_shadow.comp. A point r from the eye's nadir reads texels
// r / 256 to r / 128 wide, so from high up the land a few hundred km off
// still holds each heap's shadow. Each level lies about the nadir as it
// was when it was last baked (the finest every frame, the rest in turn:
// vulkan/sky/scene_clouds.cpp), at most 12.
#define CL_SHADOW_LEVELS    11
#define CL_SHADOW_SIDE      512
#define CL_SHADOW_TEXEL0    60.0
#define CL_SHADOW_GROWTH    2.0
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
// time jumps, and otherwise every CL_CACHE_SLICES-th row of a level at a
// time, as many a frame as the wind's time since owes, each row again every
// CL_CACHE_REFRESH seconds of it (the wind, the sun's motion).
#define CL_CACHE_LEVELS     13
#define CL_CACHE_SIDE       128
#define CL_CACHE_BINS       32
#define CL_CACHE_TEXEL0     62.5
#define CL_CACHE_MARGIN     62.0     // cells about the nadir a level's lookups stay within
#define CL_CACHE_SLICES     16
#define CL_CACHE_REFRESH    0.5333   // (16 frames at 30 a second, as when a frame made a slice of every level)
#define CL_CACHE_REACH      8        // cells across the distance to cloud is sought over
#define CL_CACHE_GROUP      8

// The heaps' map about the eye (sky/cloud_heaps.glsl): the heaps of the
// engine's lattice as the march reads them at every distance, so a heap
// seen from far is the heap flown through. On the cache's chart, read with
// the drifted point (cloudDrift: the heaps drift with the wind, the map's
// texels do not move under them), CL_HEAP_LEVELS square windows of
// CL_HEAP_SIDE texels about the drifted nadir, level k's texel
// CL_HEAP_TEXEL0 * 2^k metres, stored toroidally as the cache's are, a
// level a layer. A texel: the heaps' column over its centre
// (cloudHeapColumn): x the bottom (m up the deck), y how high the top
// stands over it, z how far the centre is inside the heaps' outline across
// (negative: outside), w how far inside a metre under the top or over the
// bottom is. Baked whole when the
// deck, the chart or the time jumps, else the texels a window takes in as
// it moves: the heaps do not change as they drift.
#define CL_HEAP_LEVELS      8
#define CL_HEAP_SIDE        1024
#define CL_HEAP_TEXEL0      16.0
#define CL_HEAP_MARGIN      480.0    // texels about the nadir a level's lookups stay within
#define CL_HEAP_GROUP       8

// The shells: the heaps past the march (setClouds' heaps.shells metres off,
// handed over across CL_SHELL_BAND times that), each drawn as itself: its
// lobes traced as solids at each pixel of a box about them, lit as the
// march lights a face (sky/cloud_shells.vert, .frag). Each frame the plan
// (sky/clouds_shells.comp, the world's module) lists the lattice's cells in
// view past the hand-over that hold a heap, CL_SHELL_VEC4S vec4s a heap,
// binned by distance (CL_SHELL_PER_OCTAVE bins each doubling of it, from the
// hand-over out), and the sort (sky/clouds_shells_sort.comp) lays them out
// farthest first, so each is drawn over those behind it. The buffer: a head
// of CL_SHELL_HEAD uints (the count; the draw's arguments; each bin's count,
// first slot and cursor), then CL_SHELL_MOST heaps in the order drawn, then
// as many as the plan wrote them.
#define CL_SHELL_MOST       32768
#define CL_SHELL_VEC4S      9
#define CL_SHELL_BINS       128
#define CL_SHELL_PER_OCTAVE 24.0
#define CL_SHELL_BAND       1.5
#define CL_SHELL_GROUP      8
#define CL_SHELL_SORT_GROUP 64
#define CL_SHELL_H_COUNT    0
#define CL_SHELL_H_ARGS     4        // VkDrawIndirectCommand
#define CL_SHELL_H_BIN      8
#define CL_SHELL_H_START    (CL_SHELL_H_BIN + CL_SHELL_BINS)
#define CL_SHELL_H_CURSOR   (CL_SHELL_H_START + CL_SHELL_BINS)
#define CL_SHELL_HEAD       (CL_SHELL_H_CURSOR + CL_SHELL_BINS)
#define CL_SHELL_VERTS      36       // a box's triangles

// The globe: the deck's columns over the whole body, for the layer the
// march reads from far over the deck where the cache's cells are coarser
// than the globe's (its windows are about the eye's nadir, so most of a
// planet seen from orbit lies in its coarsest, cells some hundreds of
// kilometres wide). A cube as the cover's, CL_GLOBE_SIDE texels a face
// with a border, each texel the world's cloudDensity at the texel's
// footprint over CL_GLOBE_BINS heights, the engine's forms' mean of it,
// and the column as a layer of that (x its optical depth up the deck, y
// the mean depth from its top of what it sends up, z the mean height of
// that, as the cache's columns). Baked whole when first read and when the
// deck or the time jumps, every CL_GLOBE_SLICES-th row a frame while the
// eye is over the deck.
#define CL_GLOBE_SIDE       1024
#define CL_GLOBE_BINS       24
#define CL_GLOBE_SLICES     128
#define CL_GLOBE_GROUP      8

// The volume about the eye (vulkan/sky/scene_cloud_volume.h): the engine's
// forms as the march would draw them, baked into a clipmap of 3D grids
// centred on the eye, so a march reads a cell instead of evaluating them.
// Its cells lie on a frame fixed in the clouds' drifted frame (cloudDrift:
// the wind a translation), a tangent frame at an anchor on the deck's
// bottom near the eye (x, y along it, z up there), so the wind moves no
// cell: what drifts is the world's base under the forms, which the bakes'
// rotation of slices keeps up with. CL_VOL_LEVELS levels of CL_VOL_SIDE^2 x
// CL_VOL_DEPTH cells, level k's cell CL_VOL_CELL0 * 2^k metres, each a
// window about the eye's cell (vertically: about the deck where the window
// is deeper than it, else about the eye held inside the deck), a cell
// stored at its index modulo the sides (toroidally: a window that moves
// bakes only the cells it takes in). A texel: x the forms' extinction
// (1/m) at the cell's footprint (clFormsAt at foot = the cell), y how far
// (m) any point of the cell is sure to be from a cell that may hold cloud
// as the march draws it at any footprint up to its own (the cells' boxes'
// gap out to CL_VOL_REACH cells, a level's), z unused (0), w whether the
// cell may hold cloud (the distance's occupancy). A level is baked whole
// when the deck, the anchor or the time jumps, the cells a window takes in
// as it moves, and every frame CL_VOL_SIDE / CL_VOL_SLICES rows of every
// level (the world's base under the drift).
#define CL_VOL_LEVELS       5
#define CL_VOL_SIDE         128
#define CL_VOL_DEPTH        64
#define CL_VOL_CELL0        32.0
#define CL_VOL_SLICES       16
#define CL_VOL_REACH        8
#define CL_VOL_MARGIN       2        // cells inside a window's edge a lookup stays
#define CL_VOL_GROUP        4        // a bake's group: CL_VOL_GROUP^3 cells
#define CL_VOL_CHECK_RAYS   4096     // the check's rays (a ray an invocation)
#define CL_VOL_CHECK_FLOATS 8        // its results a ray

#endif
