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
 *     each texel: the base the march reads (where the cell is about as
 *     wide as a cloud or wider, a share the engine cuts into clouds by its
 *     own noise), the forms' mean extinction over the cell, whether any of
 *     the cell may hold cloud as a view however near sees it, the deck's
 *     depth from it toward the sun and up, the distance to the nearest cell
 *     that may; with each column its band (the heights about it that may
 *     hold cloud) and the
 *     column as a layer (its depth, and the mean depth and height of what it
 *     sends up). A level is baked whole when its window moves, all of them
 *     when the deck, the time or the sun jumps, and a sixteenth of every
 *     level's rows each frame (the wind, the sun's motion). The world's
 *     function never runs per pixel.
 *   - THE SKIN. The engine's tiling noise moves the world's surface by a
 *     bounded depth only (a fifth of the base's rise from clear to cloud,
 *     at most `0.11 * forms.edge` out and `0.2 * forms.edge` in): Perlin-
 *     Worley billows (`forms.billowScale` a tile) for its lobes, and near
 *     the eye finer Worley erosion (`forms.detailScale`): wisps under a
 *     cloud's base, cauliflower over its middle. It never opens a hole
 *     through a cloud, nor makes one in clear air. Where the skin is under
 *     a pixel the surface is blurred over the footprint (soft edges from
 *     afar), and the detail fades out as it gets finer than a pixel;
 *     everything drifts with `wind`.
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
 *     of the cloud. From above, where the deck is a few pixels thick or
 *     less (orbit, the far deck from the limb), each pixel reads the
 *     column's layer instead, cut to the pixel by the billows: soft-edged
 *     cloud from orbit for a few texture reads.
 *   - THE LIGHT. The sun through the air (the atmosphere's transmittance)
 *     and through the deck toward it (the cache's depth, out to hundreds
 *     of kilometres; never a march toward the sun per pixel), a dual-lobe
 *     phase with a forward
 *     lobe, multiple scattering as Wrenninge's octaves (dimmed at a cloud's
 *     thin edge), the sky's light from above (less what the cloud over the
 *     point hides) and the ground's from below, and the air before the deck
 *     applied as the land gets it.
 *   - THE SHADOW. The deck's shadow on the land, plants, grass and every
 *     lit mesh, through the shared sun term (common/scene_lighting.glsl):
 *     levels of a map on a plane at the eye's nadir, each twice the last,
 *     read from the cache's depth toward the sun.
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
 * The deck's state, or undefined without one.
 * @returns {{ enabled: boolean, compiled: boolean, failed: boolean,
 *             covered: boolean, generation: number } | undefined}
 *   compiled: the module's shaders are built; failed: they did not compile
 *   (the console has the error); covered: the cover's cube is baked;
 *   generation: counts setClouds calls.
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
 *       the share of the footprint they fill (0..1) instead: the engine
 *       cuts a share into clouds of its own at the cache's coarse levels.
 *       Baked into the cache only (q.footprint 62.5 m at least), never per
 *       pixel. Zero outside the shell is the engine's.
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
