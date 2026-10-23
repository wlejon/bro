/**
 * Clouds — a world's cloud deck (bro.scene).
 *
 * `scene.setClouds(opts)` sets one deck of volumetric cloud: a shell about
 * the body (`scene.setBody`) from `bottom` to `top` metres over its datum.
 * What the clouds ARE belongs to the world: its MODULE, GLSL that says how
 * much of the sky they cover where (cloudCover) and how dense they are at a
 * point of the deck (cloudDensity). The engine owns the rest:
 *
 *   - THE COVER. cloudCover is run once over a cube about the body when the
 *     deck is set (1024 a face, about 10 km a texel on an Earth, four
 *     samples a texel), so it may be as costly as it likes: FastNoise2
 *     trees' `.glsl()`, the land's processes' grid (cloudLandGrid), a
 *     world's weather systems from its data.
 *   - THE CACHE. cloudDensity baked about the eye's nadir on a chart fixed
 *     to the body: 13 levels of 128 x 128 columns, 32 bins up the deck,
 *     the finest cell 62.5 m and each level's twice the last's; with each
 *     texel the deck's depth from it toward the sun and up, the distance
 *     to the nearest cloud, and each column's band (the heights about it
 *     that hold cloud). A level is baked whole when its window moves, all
 *     of them when the deck, the time or the sun jumps, and a sixteenth of
 *     every level's rows each frame (the wind, the sun's motion).
 *   - THE MARCH. Every pixel marches its view through the deck shell
 *     against the scene's depth, at native resolution, on a grid of steps
 *     anchored at the eye (the same for every pixel, so nothing crawls).
 *     The air is crossed on the cache (its bands a cell at a time, its
 *     distance to cloud), so a clear sky and the gaps between clouds cost
 *     a few samples. The cloud is the cache's where its cell is as fine as
 *     the pixel, else cloudDensity's at the pixel's footprint, a step cut
 *     into leaves a few pixels long. Orbit, the limb, the deck from above,
 *     inside it and from the ground looking up are one path.
 *   - THE LIGHT. The sun through the air (the atmosphere's transmittance)
 *     and through the deck toward it (the cache's depth: kilometres of it,
 *     so a tower's body shades its far side and its base), a dual-lobe
 *     phase with a forward lobe, multiple scattering as Wrenninge's
 *     octaves (dimmed at a cloud's thin edge), the sky's light from above
 *     (less what the cloud over the point hides) and the ground's from
 *     below, and the air before the deck applied as the land gets it.
 *   - THE SHADOW. The deck's shadow on the land, plants, grass and every
 *     lit mesh, through the shared sun term (common/scene_lighting.glsl):
 *     levels of a map on a plane at the eye's nadir, each twice the last,
 *     read from the cache's depth toward the sun.
 *
 * Noise-free under a pixel: the module is asked for the density's MEAN over
 * `q.footprint` (below), the engine's noise is mipmapped and carries its own
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
 * @typedef {Object} CloudsOptions
 * @property {string|{source: string}} module  GLSL defining cloudCover and
 *     cloudDensity (the contract below). Required unless `enabled` is false.
 * @property {number} [bottom=1500]  the deck's bottom, metres over the datum
 * @property {number} [top=8000]     its top (must be over `bottom`)
 * @property {number} [albedo=0.98]  the cloud's single-scattering albedo
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
 *       Extinction (1/m) at a point of the deck: cumulus about 0.02..0.1, a
 *       thin veil 0.001. Called for every texel of the cache (q.footprint
 *       its cell) and for the march's samples of cloud nearer than the
 *       cache resolves (q.footprint the pixel's): the costly one. Read q.cover and the
 *       engine's noise, and return the MEAN over q.footprint: detail finer
 *       than the footprint must be at its mean, or what a pixel or a shadow
 *       texel covers will alias. Zero outside the shell is the engine's.
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
 *   vec4  cloudNoise(vec3 x, float fp)   one tile of tiling noise at `x` in
 *         tiles (a tile is whatever length the world divides by), `fp` the
 *         footprint in tiles (its mips give the mean beyond): x Perlin-Worley
 *         (a cloud's body), y Worley fbm at 4 cells a tile, z at 8 (the
 *         detail that erodes edges), w x squared (x's variance in the
 *         footprint).
 *   float cloudNoiseAbove(vec4 n, float t)  the share of n's footprint where
 *         n.x is over t: all or nothing up close, the covered share from afar.
 *         Cutting clouds out with it (rather than with step/smoothstep on
 *         n.x) keeps a field of cumulus seen from orbit as grey as its
 *         clouds and gaps make it, not a uniform sheet.
 *   float cloudRemap(float v, float lo, float hi, float a, float b)
 *   vec4  cloudLandGrid(int c, vec3 up)  channel c of the land's processes'
 *         grid at `up` (in cloudCover only; zero without `land`)
 *
 * Example (a deck covering a third of the sky in cumulus):
 *
 *   scene.setClouds({ bottom: 1500, top: 4000, module: `
 *       vec4 cloudCover(vec3 up) { return vec4(0.35, 0.0, 0.0, 0.0); }
 *       float cloudDensity(CloudPoint q) {
 *           vec4 n = cloudNoise(q.p / 6000.0, q.footprint / 6000.0);
 *           float grad = smoothstep(0.0, 0.1, q.height) * smoothstep(1.0, 0.4, q.height);
 *           float share = cloudNoiseAbove(n, 1.0 - q.cover.x * grad);
 *           return 0.04 * share;
 *       }` });
 *
 * The planet demo (broworkshop demos/planet/world.js, "the clouds") is a
 * full module: cover from its climate and the land's wet belts, frontal
 * bands and cyclones from its data, cumulus fields with gaps, and the
 * density's footprint mean taken as a transmittance mean.
 */
