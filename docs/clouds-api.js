/**
 * Clouds — a world's cloud deck (bro.scene).
 *
 * `scene.setClouds(opts)` sets one deck of volumetric cloud: a shell about
 * the body (`scene.setBody`) from `bottom` to `top` metres over its datum.
 * The work is split the Nubis way. The WORLD gives the clouds' BASE SHAPE:
 * its MODULE, GLSL that says how much of the sky they cover where
 * (cloudCover) and, at a point of the deck, the clouds themselves as a
 * level set (cloudDensity: each heap and slab, their surface at 0.5, under
 * 0 the distance to the nearest). The ENGINE gives that surface its SKIN
 * and owns everything after:
 *
 *   - THE COVER. cloudCover is run once over a cube about the body when the
 *     deck is set (1024 a face, about 10 km a texel on an Earth, four
 *     samples a texel), so it may be as costly as it likes: FastNoise2
 *     trees' `.glsl()`, the land's processes' grid (cloudLandGrid), a
 *     world's weather systems from its data.
 *   - THE CACHE. cloudDensity is baked about the eye's nadir on a chart
 *     fixed to the body: 13 levels of 128 x 128 columns, 32 bins up the
 *     deck, the finest cell 62.5 m and each level's twice the last's. With
 *     each texel: the slabs' base the march reads (where the cell is about
 *     as wide as a cloud or wider, the share they fill: the world's, never
 *     the engine's clouds), the heaps' (the heaps themselves where the
 *     cell is finer than them, else the world's share of them), how much
 *     of the skin's move shows there (none under a flat base), the forms'
 *     mean extinction over the cell (slabs and heaps), whether any of
 *     the cell may hold cloud as a view however near sees it, the deck's
 *     depth from it toward the sun and up, the distance to the nearest cell
 *     that may; with each column its band (the heights about it that may
 *     hold cloud) and the
 *     column as a layer (its depth, and the mean depth and height of what it
 *     sends up). A level is baked whole when its window moves, all of them
 *     when the deck, the time or the sun jumps, and a sixteenth of every
 *     level's rows each frame (the wind, the sun's motion). The world's
 *     function never runs per pixel.
 *   - THE HEAPS (with `heaps`). The world's cumulus as things: one about
 *     each cell of a lattice the engine lays on the cover's cube, its lobes
 *     the world's (cloudHeap). The engine bakes them into a map about the
 *     eye's drifted nadir (8 levels of 1024 x 1024 texels, the finest 16 m
 *     and each level's twice the last's): each texel the heaps' column over
 *     it (its bottom, how high its top stands, how far it is inside their
 *     outline, its top's slope; past their outline, the nearest's outline's
 *     height and slope there). A point's distance inside the heaps is
 *     read from each texel's column about it and blended between them (in
 *     metres, as from a lobe's ellipsoid: over a top, from a dome about the
 *     heaps' size), so their surface goes on smoothly from texel to texel.
 *     The march reads its base there at every
 *     distance, so a heap seen from 400 km off is the heap flown through,
 *     the same heap in the same place, refined as it nears, never a field
 *     the engine made up; where they come under a few pixels across, their
 *     share in the cache (the world's cloudHeapsShare) takes over, so from
 *     orbit a field of them is the veil they make. They drift with the
 *     wind, the map with them: baked whole when the deck or the chart
 *     changes, else the texels a window takes in as the eye moves.
 *   - THE GLOBE. While the eye is over the deck, the whole deck as columns
 *     on a cube about the body (1024 texels a face, some 10 km each): the
 *     world's base at the texel's footprint (the share its clouds fill,
 *     never cut into the engine's), each column a layer seen from above.
 *     A layer's depth is the clouds' cover over the column, not the sum of
 *     its heights' (a cloud stands over many: they overlap), so a field of
 *     heaps that fill a third of the air at each height is a veil a third
 *     as opaque from above however deep it is; the cache's columns are
 *     made the same way. Baked whole when the deck or the time jumps, a
 *     128th of its rows each frame.
 *   - THE SKIN. The engine's tiling noise moves the world's surface by a
 *     bounded depth only (a fifth of the base's rise from clear to cloud,
 *     at most `0.11 * forms.edge` out and `0.2 * forms.edge` in): Perlin-
 *     Worley billows (`forms.billowScale` a tile) for its lobes, and near
 *     the eye finer Worley erosion (`forms.detailScale`): wisps under a
 *     cloud's base, cauliflower over its middle. It never opens a hole
 *     through a cloud, nor makes one in clear air. Where the base rises
 *     steeply up (a flat underside) the skin moves it not at all, so a
 *     cumulus' base stays a flat grey plane and its lobes are on its walls
 *     and top. The skin shows over a pixel's footprint: as its lobes come
 *     under four pixels across its move fades out (none at two) into a
 *     softer edge, so a heap seen small is the world's smooth dome, lit on
 *     its sun side and shaded in its lee, never grain; the detail fades out
 *     the same way. The engine adds no clouds of its own: what it moves is
 *     the world's slabs' surface and the lattice's heaps'. Everything
 *     drifts with `wind`.
 *   - THE MARCH. Every pixel marches its view through the deck shell
 *     against the scene's depth, at native resolution, with no jitter (the
 *     same picture every frame). A cumulus is a soft solid with a skin, so
 *     the view is traced: through clear air it steps by distances sure to
 *     hold no cloud (the cache's bands and distance to cloud, made from
 *     where each cell may hold any however near, and the base's own: how
 *     far it must still rise, at the fastest the contract lets it, before
 *     even the skin's most out reaches the surface), so no cloud is skipped
 *     and no edge torn; within the skin's depth it steps a few pixels at a
 *     time, finds a cloud's face by false position on the forms' signed
 *     field (so it moves with the cloud and not with the steps), and inside
 *     crosses pieces growing from the face until the view is spent or out
 *     of the cloud. From above, where a pixel spans a fortieth of the
 *     deck's thickness or more (orbit, the limb's, a climb from a region's
 *     height), each pixel reads a column's layer instead: the cache's,
 *     cut to the pixel by the billows, where its cells are finer than the
 *     globe's and the pixel finer than the clouds; else the globe's, the
 *     footprint's mean of what the march would show (a field of heaps
 *     under a pixel a soft veil at its cover, an overcast's edge soft over
 *     the footprint). Where the heaps are a few pixels across or more, the
 *     layer is the slabs' column and over it the heaps' tops from their
 *     map (opaque, lit by their slope toward the sun), the heaps the march
 *     draws below. A few texture reads a pixel, and the same clouds at
 *     every height on the way up. An opaque layer's top is lit as a lumpy
 *     surface, not flat: the billows' noise at 16 edges a tile (its lumps
 *     some 2 edges across) as a relief 0.8 edges high, its slope toward the
 *     sun brightening the sun-facing sides and greying the lee, faded out
 *     as the lumps come under a few pixels. A world that shapes its slabs'
 *     tops by the same noise at the same tile and height (cloudNoise(p /
 *     (16 * edge)).x, 0.8 edges down where it is low) is lit as it is
 *     shaped; one whose tops are flat gets the lumps in the light alone.
 *   - THE LIGHT. The sun through the air (the atmosphere's transmittance)
 *     and through the deck toward it: where the view meets a cloud, its
 *     own forms over the first few hundred metres toward the sun (four
 *     reads, once a face: a heap's sunward side lit, its far side and
 *     underside grey, the skin's creases shadowed by the lobes about them),
 *     then the cache's depth out to hundreds of kilometres; a dual-lobe
 *     phase with a forward lobe (the silver lining), multiple scattering as
 *     Wrenninge's octaves (dimmed at a cloud's thin edge), the sky's light
 *     from above (less what the cloud over the point hides) and the
 *     ground's from below, and the air before the deck applied as the land
 *     gets it.
 *   - THE SHADOW. The deck's shadow on the land, plants, grass and every
 *     lit mesh, through the shared sun term (common/scene_lighting.glsl):
 *     levels of a map on a plane at the eye's nadir, each twice the last
 *     (512 texels a side from 60 m, so land r from the nadir holds texels
 *     r / 256 to r / 128 wide: each heap shades the land under it from
 *     high up too), read from the cache's depth toward the sun and the
 *     heaps' map. The finest two are baked every frame, the rest one a
 *     frame in turn, each about the nadir as it was when baked (sooner
 *     when the eye has moved 8 of its texels).
 *
 * Noise-free under a pixel: the base is baked as its mean over each cell
 * (`q.footprint`), the engine's noise is mipmapped and carries its own
 * variance (cloudNoiseAbove), so a cloud seen from orbit, its shadow and the
 * same cloud from a kilometre agree, and nothing sparkles.
 *
 * A scene without a deck pays nothing and renders exactly as before; a
 * module that does not compile reports `failed` (cloudStats) and the scene
 * draws as without one.
 *
 * Design: docs/scene-world-architecture.md §13 item 4, §7, §11 "Going up".
 */

/**
 * @typedef {Object} CloudForms
 * @property {number} [extinction=0.06]  a cloud's heart's extinction (1/m):
 *     what a full billow takes out of the view a metre
 * @property {number} [billowScale=4000]  the billows' tile (m): how wide the
 *     engine's Perlin-Worley noise repeats; a lobe of the skin is about a
 *     tenth of it
 * @property {number} [detailScale=1200]  the erosion's tile (m)
 * @property {number} [edge=600]  the metres over which the world's base
 *     rises from clear (0) to cloud (1) at the fastest: the skin's depth is
 *     a fifth of it, and the march's steps through clear air are as long as
 *     the base says the cloud is far in these units. About the least a
 *     cloud's size; a cell as wide as it reads the base as a share
 */

/**
 * @typedef {Object} CloudsOptions
 * @property {string|{source: string}} module  GLSL defining cloudCover and
 *     cloudDensity (the contract below). Required unless `enabled` is false.
 * @property {number} [bottom=1500]  the deck's bottom, metres over the datum
 * @property {number} [top=8000]     its top (must be over `bottom`)
 * @property {number} [albedo=0.98]  the cloud's single-scattering albedo
 * @property {CloudForms} [forms]    the engine's forms the base is cut into
 * @property {ArrayLike<number>} [wind=[0,0,0]]  the wind (m/s, the body's
 *     frame) the forms drift with; a module that moves its base reads
 *     cloudDrift(p) to move with them
 * @property {{cell: number}} [heaps]  the heaps' lattice: the engine lays
 *     cells `cell` metres wide (100 or more) on the cover's cube at the
 *     deck's bottom, drifting with the wind, and asks the module's
 *     cloudHeap (with `#define CLOUD_HEAPS`) for the heap each holds: its
 *     lobes, half-ellipsoids on flat bases, within a cell of the cell's
 *     middle (sky/scene_cloud_layer.glsl has the types), and its
 *     cloudHeapsShare for their mean where they are finer than the
 *     footprint; cloudDensity then gives the rest (slabs) at every
 *     footprint (THE HEAPS). One heap at every distance: the one a view
 *     from far off sees is the one flown through. Without it (or 0) the
 *     module's cloudDensity is all.
 * @property {boolean} [shadows=true]  the deck shadows what the sun lights
 * @property {LandNode|number} [land]  a round land whose processes' grid the
 *     cover's bake reads (cloudLandGrid); without one it reads zero
 * @property {ArrayLike<number>} [data]  the world's data, read in GLSL as
 *     cloudData(i): vec4s, at most 64 (256 numbers)
 * @property {boolean} [enabled=true]  false removes the deck
 */

/**
 * Set (or replace) the scene's cloud deck. Every call replaces the whole
 * deck and bakes the cover again; `null` or `{ enabled: false }` removes it.
 * Throws a TypeError for a missing module, `top <= bottom` or a negative `bottom`.
 * @param {CloudsOptions|null} opts
 */
SceneGraph.prototype.setClouds = function (opts) {};

/**
 * @typedef {Object} CloudVolumeCheck
 * The volume's newest self-check (only with BRO_CLOUD_VOLUME_CHECK=1 in the
 * environment: a frame in 30, `rays` rays from the eye over every
 * direction, each walked through the deck on the volume as a march would).
 * @property {number} serial      the check's number
 * @property {number} rays
 * @property {number} compared    points where the volume's extinction was
 *     compared with the forms' at the level's cell footprint
 * @property {number} meanAbsError  the mean absolute difference (1/m)
 * @property {number} meanSigma   the forms' mean extinction over those
 *     points (1/m), and `mostSigma` the most: the scale of the error
 * @property {number} mostSigma
 * @property {number} jumps       jumps taken by the volume's distance, and
 *     `meanJump` their mean length (m)
 * @property {number} meanJump
 * @property {number} inside      points sampled inside the jumps, the forms
 *     read as the march reads them there (at its pixel's footprint, with
 *     the detail and without) and at the cell's
 * @property {number} violations  of those, points where the forms hold
 *     cloud: a jump past cloud the march would draw (0 when conservative)
 */

/**
 * @typedef {Object} CloudVolumeStats
 * The volume about the eye: the engine's forms as the march draws them,
 * baked into a clipmap of 3D grids centred on the eye (in the clouds'
 * drifted frame, so the wind moves no cell): per cell the forms'
 * extinction at the cell's footprint, a conservative distance (m) to the
 * nearest cell that may hold cloud as the forms draw it at any footprint up
 * to the cell's. Baked a share of every level's rows a frame, the cells a
 * window takes in as the eye moves, a level whole when the deck or its time
 * jumps. The march reads its extinction deep in a cloud; the light toward
 * the sun is the forms' and the cache's, never the volume's.
 * @property {number} levels      the clipmap's levels (each twice the last's cell)
 * @property {number} side        cells a level's window spans across
 * @property {number} depth       cells it spans up
 * @property {number} cell        the finest level's cell (m)
 * @property {number} pending     bakes still to come for this view (levels
 *     to bake whole, and slices of rows of a round of them since the last
 *     whole change or the last frame the cache still changed: a cell's
 *     distance reads the rows about it, which each slice's distance passes
 *     make again, so a round from a settled cache is final): the settle
 *     state, "clouds: N volume bakes to come"
 * @property {number} bytes       its GPU memory
 * @property {number} cellsBaked  the cells whose forms the newest frame baked
 * @property {number} slack       metres its distances are read less: how far
 *     the world's base can have drifted under the forms since the oldest
 *     rows were baked
 * @property {CloudVolumeCheck} [check]
 */

/**
 * The deck's state, or undefined without one.
 * @returns {{ enabled: boolean, compiled: boolean, failed: boolean,
 *             covered: boolean, generation: number,
 *             volume: CloudVolumeStats } | undefined}
 *   compiled: the module's shaders and the deck's pipelines are built
 *   (compiled off the frame thread, so the deck is not drawn for the frames
 *   before; docs/code-cache.md "Shader warm-up"); failed: they did not compile
 *   (the console has the error); covered: the cover's cube is baked (a face
 *   a frame, the deck drawn once all six are; a later bake, for a new
 *   generation, keeps the deck drawn with the faces not yet done as they were);
 *   generation: counts setClouds calls; volume: the volume about the eye.
 *
 * Debugging the volume (environment variables, read once at start):
 * BRO_CLOUD_VOLUME_DEBUG=1 draws the deck from the volume alone instead of
 * the forms (to judge it against the real clouds); =2[:<m>] its channels as
 * slices from above at <m> metres over the deck's bottom (default 1000), a
 * panel a level left to right and a channel top to bottom (extinction,
 * distance, occupancy) in false colour; =3 the same as
 * upright slices through the eye. BRO_CLOUD_VOLUME_CHECK=1 runs the check
 * (`volume.check`, also logged).
 */
SceneGraph.prototype.cloudStats = function () {};

/*
 * THE MODULE CONTRACT (GLSL; src/scene/vulkan/shaders/sky/scene_cloud_layer.glsl)
 *
 *   vec4 cloudCover(vec3 up)
 *       The deck's large-scale state at body direction `up` (unit, the
 *       body's frame). x how much of the sky its clouds cover there (0..1);
 *       y, z, w the world's own (a cloud type, how high its tops reach...).
 *       Each in 0..1 (stored rgba16f). Baked once (see THE COVER).
 *
 *   float cloudDensity(CloudPoint q)
 *       The deck's BASE at a point of it: the clouds as a level set, their
 *       surface at 0.5 (before the engine's skin), 1 a cloud's heart. It
 *       must rise no faster than 1 over `forms.edge` metres any way, or (a
 *       cloud's flat base or a slab's top) 3 over `edge` up or down and a
 *       fifth of that across: the march steps through clear air by it. In
 *       clear air it goes on under 0 as the distance to cloud says
 *       (0.5 - distance / edge), so a view crosses the gaps between heaps
 *       in a few steps; a module that clamps it at 0 is still right, only
 *       slower. Where q.footprint is as wide as the clouds or wider, give
 *       the share of the footprint they fill (0..1) instead: the cache's
 *       coarse levels and the globe read it as it is, so it must be the
 *       footprint's mean (a share that thresholds a noise finer than the
 *       footprint reads from orbit as hard-edged patches). Baked into the
 *       cache (q.footprint 62.5 m at least) and the globe (some 10 km),
 *       never per pixel. Zero outside the shell is the engine's. With
 *       heaps (below), the slabs alone.
 *
 *   #define CLOUD_HEAPS, with
 *   bool cloudHeap(CloudHeapCell c, out CloudHeap h)
 *       The heap cell `c` of the engine's lattice holds (setClouds' heaps):
 *       up to four lobes, half-ellipsoids on flat bases, all of them within
 *       a cell of the cell's middle; false none. Asked once a cell by the
 *       heaps' map's bake and the cache's, never per pixel.
 *   float cloudHeapsShare(CloudPoint q)
 *       The share of the air the heaps fill at `q` on the mean over its
 *       footprint (read where it is as wide as they are), and above 0,
 *       however little, wherever a heap may stand, under 0 where none
 *       can: the engine's bounds trust it to pass no heap by.
 *
 *   struct CloudPoint {
 *       vec3 p;          // the point in the body's frame (m from its centre)
 *       vec3 up;         // its direction (unit)
 *       float height;    // 0 at the deck's bottom .. 1 at its top
 *       float altitude;  // metres over the datum
 *       vec4 cover;      // cloudCover at `up`, bilinear from the cube
 *       float footprint; // metres: what is finer than this is at its mean
 *       float time;      // the scene's clock (s), for drift and growth
 *   };
 *
 * The engine gives the module:
 *
 *   vec4  cloudData(int i)               the world's data (setClouds' `data`)
 *   vec3  cloudDrift(vec3 p)             `p` carried back by the wind to where
 *         the engine's forms are now (p - wind * time): a base read at it
 *         moves with its clouds
 *   vec4  cloudNoise(vec3 x, float fp)   one tile of tiling noise at `x` in
 *         tiles (a tile is whatever length the world divides by), `fp` the
 *         footprint in tiles (its mips give the mean beyond): x Perlin-Worley,
 *         y Worley fbm at 4 cells a tile, z at 8, w x squared (x's variance
 *         in the footprint).
 *   float cloudNoiseAbove(vec4 n, float t)  the share of n's footprint where
 *         n.x is over t: all or nothing up close, the covered share from afar.
 *   float cloudRemap(float v, float lo, float hi, float a, float b)
 *   vec4  cloudLandGrid(int c, vec3 up)  channel c of the land's processes'
 *         grid at `up` (in cloudCover only; zero without `land`)
 *
 * Example (one slab 1 km thick, its surface 600 m from clear to cloud):
 *
 *   scene.setClouds({ bottom: 1500, top: 4000,
 *       forms: { billowScale: 4000, detailScale: 1200, edge: 600 },
 *       wind: [6, 0, 0],
 *       module: `
 *       vec4 cloudCover(vec3 up) { return vec4(1.0, 0.0, 0.0, 0.0); }
 *       float cloudDensity(CloudPoint q) {
 *           // metres inside the slab (2000 .. 3000 m), at most a third of
 *           // the edge's rate up and down
 *           float inside = min(q.altitude - 2000.0, 3000.0 - q.altitude) * 3.0;
 *           return min(0.5 + inside / 600.0, 1.0);
 *       }` });
 *
 * The planet demo (broworkshop demos/planet/world.js, "the clouds") is a
 * full module: cover from its climate and the land's wet belts, frontal
 * bands and cyclones from its data, discrete cumulus heaps on a cellular
 * layout a few kilometres apart where the cover is broken (each a dome
 * with its own flat base and top, clear air between), slabs with their own
 * relief where it is overcast, and the share of each far off.
 */
