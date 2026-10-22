/**
 * Land — a world's ground from underfoot to the horizon (bro.scene).
 *
 * `scene.createLand(opts)` makes a land node. What the land IS belongs to the
 * world: its height, a FastNoise node tree (docs/noise-api.js), and its
 * surface, GLSL. The engine owns the rest:
 *
 *   - TILES. The height tree is run as GLSL (`node.glsl()`) together with
 *     the surface, the world's land MODULE, in a compute pass over tiles of
 *     the land as the view needs them (each tile's texel about the same
 *     size on screen near and far), made by the world service as the
 *     frame's GPU slack allows and kept until their slots are wanted.
 *   - RINGS. The land is drawn as concentric squares of fixed topology
 *     parked on the camera, each ring's cells twice the last's, placed on
 *     the land from the tiles in the vertex stage. Crack-free and pop-free
 *     by construction: a vertex's height is a function of where it is and
 *     how far from the eye, never of which ring it belongs to.
 *   - THE CPU'S HEIGHTS. `landHeight`, `sampleHeights`, raycasts, a grass
 *     node's bounds and placement read the same tree, run by FastNoise2
 *     itself: exact, with nothing read back from the GPU per query.
 *   - PROCESSES. Shape a point at a time cannot make (drainage that
 *     connects, ridges between basins, erosion) comes from the world's
 *     `processes`: GLSL kernels the engine runs over a grid of the land when
 *     it is made (below). Their first channel is the height over the tree's
 *     (x added, y the tree's scale), on the GPU and, read back once, on
 *     the CPU.
 *   - WHAT GROWS. A grass node stands on a land (`createGrass({ ground:
 *     land })`) and reads the land's tiles on the GPU: its heights and the
 *     surface's `cover` and `herbs`, so the grass reaches as far as the
 *     land does, with nothing baked on the CPU. A plants node standing on
 *     it (`createPlants({ land })`, vegetation-api.js) puts the canopy its
 *     rules make in the land's tiles: past the plants' range the land rises
 *     by it and is drawn as their leaves, past their shadow range it shades
 *     the land as they would, and a forest reaches the horizon.
 *
 * On a flat body the land is square: `extent` metres from the origin each
 * way. On a round body (`scene.setBody`, docs/scene-world-architecture.md
 * §11) it is the whole body: its tiles are keyed on the cube faces of the
 * body's chart, its rings laid out about the eye in the map round it to the
 * antipode (more rings are added as it takes; the ones finer than a pixel
 * from the eye's height are not drawn), its height a 3D tree at the body's
 * points, and its CPU heights answer in the place frame, the body's curve
 * included. Its transform is ignored. It casts shadows (a hill's on the slope behind it,
 * on the grass and the plants) and receives them. A low sun's long shade,
 * a hill's across a valley or a far forest's across a meadow, comes from
 * the land's sun: the height a point must clear to see the sun, marched
 * over the land's tiles about the eye to 16 km (the first land offered;
 * docs/scene-world-architecture.md §5).
 *
 * THE SURFACE MODULE (scene_land_layer.glsl is the contract). GLSL that
 * defines
 *
 *     LandSurface landSurface(LandPoint q)
 *
 * where `q.p` is world (x, z), `q.height` the land's height there and
 * `q.normal` its normal (from the heights); on a round body `q.at` is the
 * datum's point under it (relative to the tile's origin, which a tree's GLSL
 * adds back in double) and `q.up` the body's up there, so a world's belts
 * and snow lines are its own GLSL of them. It returns
 *
 *     struct LandSurface {
 *         vec3 albedo;   // linear: the ground where no turf covers it
 *         vec4 cover;    // what a grass node on the land grows here: x its
 *                        // sward's density (0 none .. 1), y dryness (0 lush
 *                        // .. 1 sun-dried), z height multiplier (0 .. 2),
 *                        // w the canopy overhead (0 open .. 1 closed: leaf
 *                        // litter on the turf, the herbs' light)
 *         float herbs;   // how thickly its herbs grow (0 none .. 1)
 *     };
 *
 * Start from `landSurfaceOf(albedo)`: bare ground, nothing growing (cover
 * (0, 0, 1, 0), herbs 0). It may call `landHeight(vec2 p)` (the land's
 * height: the tree plus the processes'), `landTree(vec2 p)` (the tree alone,
 * at the seed), `landNoise(vec2 p, int seed)` and `landGrid(int c, vec2 p)`
 * (the processes' channel c, zero with none; on a round body each takes the
 * vec3 datum point `q.at` instead), `landData(int i)` (the
 * node's `data`, vec4 i of at most 64), its own functions and any other
 * tree's GLSL it carries (`someNode.glsl({ name: 'cover' })`). It runs
 * once a texel of a tile as the tile is made, not every frame, so it may
 * be as elaborate as the world wants. One that does not compile is logged
 * and the land draws nothing.
 *
 * Where a grass node's turf covers the land (`grass.shadeGround(land)`), the
 * turf shades it; the surface is what shows elsewhere.
 *
 * PROCESSES (land_process_layer.glsl is the contract; docs/scene-world-
 * architecture.md §11, "Processes"). GLSL that defines
 *
 *     void procInit(inout vec4 v[PROC_CHANNELS])    // a cell's first values (v starts at 0, v[0].y at 1)
 *     void <pass>(inout vec4 v[PROC_CHANNELS])      // each pass's kernel: v starts as the cell's last
 *
 * run over the land's GRID: `resolution` cells a side over the land (on a
 * round body each of the six cube faces), `channels` vec4s a cell (1 to 4).
 * procInit runs once, then each of `passes` its `iterations` times, every
 * cell at once, reading its last values and its neighbours' with
 * `procRead(c, ivec2 offset)` (offsets within one; across a cube face's
 * edge the neighbouring face's cells). A kernel also has `procPoint()` (the
 * cell's centre: (x, 0, z) flat, the datum point on a round body),
 * `procPointAt(offset)`, `procUp()`, `procChart()`, `procRoot()`,
 * `procCellSize()`, `procIteration()`, `procData(i)` (the processes' `data`)
 * and `landTree(p)`. Channel 0 is the land's height over its tree: the
 * height is `x + y * landTree(p)`, and a cell starts at x 0, y 1 (the tree
 * alone), so a process can lay down the coarse form in x and say how much
 * of the tree's detail rides on it in y (none in a sea, more on a range); every
 * channel is the surface module's through `landGrid(c, p)` (moisture, flow,
 * rock: whatever the world computes). They run on the GPU when the land is
 * made (a new height, seed, extent, body or processes runs them again),
 * 128 iterations a frame, then channel 0 is read back once: until then the
 * land draws nothing, its heights are NaN and `landStats().ready` is false.
 * Processes that do not compile are logged and the land stays unready.
 * Between cell centres both sides read the grid bilinearly, the same way.
 *
 * @example
 *   const N = (type, props = {}) => { const n = FastNoise.create(type); for (const k in props) n.set(k, props[k]); return n; };
 *   const hills = N('Multiply', { LHS: N('FractalFBm', { Source: N('Perlin', { 'Feature Scale': 400 }), Octaves: 5 }), RHS: 60 });
 *   const cover = N('Value', { 'Feature Scale': 80 });
 *   const land = scene.createLand({
 *     height: hills, seed: 1337, extent: 2048,
 *     surface: cover.glsl({ name: 'coverNoise' }) + `
 *       LandSurface landSurface(LandPoint q) {
 *         float c = coverNoise(q.p, 1337) * 0.5 + 0.5;
 *         float rock = smoothstep(0.75, 0.6, q.normal.y);
 *         LandSurface s = landSurfaceOf(mix(mix(vec3(0.12, 0.16, 0.05), vec3(0.20, 0.17, 0.08), c),
 *                                           vec3(0.3, 0.28, 0.26), rock));
 *         s.cover = vec4(1.0 - rock, c, 1.0, 0.0);   // grass off the rock, drier where c is high
 *         s.herbs = 0.5 * (1.0 - rock);
 *         return s;
 *       }`,
 *   });
 *   land.landHeight(10, 20);                       // FastNoise2 on `hills`
 *   const grass = scene.createGrass({ ground: land, turf: true, sward, herbs });
 *
 * @example
 *   // A broad basin carved under the hills, smoothed over 40 iterations.
 *   land.setLand({ processes: { resolution: 128, data: [400], passes: [{ fn: 'relax', iterations: 40 }], source: `
 *     void procInit(inout vec4 v[PROC_CHANNELS]) {
 *       vec3 p = procPoint();
 *       v[0].x = -procData(0).x * exp(-dot(p.xz, p.xz) / (1500.0 * 1500.0));
 *     }
 *     void relax(inout vec4 v[PROC_CHANNELS]) {
 *       vec4 s = vec4(0.0);
 *       for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) s += procRead(0, ivec2(i, j));
 *       v[0] = s / 9.0;
 *     }` } });
 *   // ...frames later, land.landStats().ready: landHeight is hills plus the basin.
 */

/**
 * @typedef {Object} LandOptions
 * @property {FastNoise} height - the land's height (m) at world (x, z) (on a
 *   round body at the datum's point (x, y, z), over the datum): the tree run
 *   as `landNoise(p, seed)` on the GPU and by FastNoise2 on the CPU.
 *   Its nodes must be ones `glsl()` emits (docs/noise-api.js).
 * @property {number} [seed=0] - the seed the height runs with
 *   (`LAND_SEED` in the module).
 * @property {string|{source: string}} [surface] - GLSL defining
 *   `landSurface` (see above); default bare earth.
 * @property {ArrayLike<number>} [data] - what `landData(i)` reads: at most
 *   256 floats (64 vec4s).
 * @property {LandProcesses|null} [processes] - kernels run over the land's
 *   grid when it is made (see above); null none.
 * @property {number} [extent=4096] - the land's half-size (m); a round
 *   body's land is the whole body.
 * @property {number} [cellSize=0.5] - the finest ring's cell (m).
 * @property {number} [resolution=256] - a ring's cells a side (a multiple
 *   of 4): a cell is about 2 / resolution of the view's height on screen.
 * @property {number} [levels=10] - rings: they reach cellSize *
 *   resolution / 2 * 2^(levels - 1) metres. On a round body at least as many
 *   as reach the antipode (up to 20).
 * @property {LandSea|null} [sea] - water filling the land up to a level
 *   wherever its ground lies lower; null none (the default).
 * @property {string} [name]
 * @property {boolean} [receivesShadow=true]
 * @property {boolean} [castsShadow=true]
 * @property {boolean} [visible=true]
 */

/**
 * @typedef {Object} LandSea
 * A land's sea. Its vertices stand on the water where the ground is under
 * it; each fragment decides water or ground from its own ground height (read
 * from the tiles at the fragment's footprint), so a coast is as sharp as the
 * pixels whatever the tiles' texels. The water is shaded flat (the body's up)
 * as a dielectric: the seabed through it, dimmed by `absorb` along the light's
 * way down and back (refracted), over `scatter`, the colour deep water sends
 * back. landHeight and the plants' ground stay the seabed's. Omitted members
 * keep what they were (at first the defaults).
 * @property {number} [level=0] - the water's surface, m over the datum.
 * @property {number[]} [absorb=[0.45, 0.09, 0.05]] - what each metre of water
 *   takes from the light, red, green and blue (1/m).
 * @property {number[]} [scatter=[0.004, 0.016, 0.03]] - deep water's albedo.
 * @property {number} [roughness=0.08] - the surface's roughness.
 * @example
 *   land.setLand({ sea: { level: 0, absorb: [0.6, 0.12, 0.08] } });
 *   land.setLand({ sea: null });   // dry
 */

/**
 * @typedef {Object} LandProcesses
 * @property {string} source - GLSL defining `procInit` and each pass's
 *   function (land_process_layer.glsl).
 * @property {Array<{fn: string, iterations?: number}>} [passes] - run in
 *   order after procInit, each `iterations` times (default 1).
 * @property {number} [resolution=256] - cells a side of the grid (of the
 *   land's extent; on a round body of each cube face), at most 2048.
 * @property {number} [channels=1] - vec4s a cell, 1 to 4.
 * @property {ArrayLike<number>} [data] - what `procData(i)` reads: at most
 *   256 floats.
 */

/**
 * @typedef {Object} LandStats
 * @property {number} needed - tiles the view needs.
 * @property {number} resident - tiles held.
 * @property {number} missing - needed and not made at the land's current module.
 * @property {number} filled - tiles made this frame.
 * @property {number} levels - tile levels.
 * @property {number} shortfall - how many times too coarse the worst tile
 *   the atlas had no room to refine is (1: none).
 * @property {boolean} compiled - its module compiled (it draws).
 * @property {boolean} ready - it has no processes, or theirs have run and
 *   been read back (it draws and its heights answer).
 */

class SceneGraph {
  /**
   * @param {LandOptions} opts
   * @returns {SceneNode} the land node
   * @throws {TypeError} without `height`, or when its tree does not encode
   *   or decode.
   */
  createLand(opts) {}
}

class SceneNode {
  /**
   * Change a land: any of LandOptions. A new height, seed, surface, data,
   * extent or rings remakes every tile: the land draws nothing for the
   * frame its first new tiles are made in.
   * @param {LandOptions} opts
   * @returns {SceneNode} this
   */
  setLand(opts) {}

  /**
   * The land's height at world (x, z), metres; NaN off the land and until
   * it is ready (its processes read back). On a round
   * body (x, z) and the answer are the place frame's: the y of the ground
   * under (x, z), the body's curve included.
   * @param {number} x
   * @param {number} z
   * @returns {number}
   */
  landHeight(x, z) {}

  /**
   * Heights on a grid, (x0 + i * cell, z0 + j * cell) row by row; NaN off
   * the land. (Every mesh node has it; for a land it is the land's.)
   * @returns {Float32Array}
   */
  sampleHeights(x0, z0, cell, cols, rows) {}

  /** @returns {{extent: number, cellSize: number, resolution: number, levels: number}|undefined} */
  landParams() {}

  /** @returns {LandStats|undefined} undefined for a node that is not a land. */
  landStats() {}
}
