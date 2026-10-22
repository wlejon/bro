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
 *     itself: exact, with nothing read back from the GPU.
 *   - WHAT GROWS. A grass node stands on a land (`createGrass({ ground:
 *     land })`) and reads the land's tiles on the GPU: its heights and the
 *     surface's `cover` and `herbs`, so the grass reaches as far as the
 *     land does, with nothing baked on the CPU. A plants node standing on
 *     it (`createPlants({ land })`, vegetation-api.js) puts the canopy its
 *     rules make in the land's tiles: past the plants' range the land rises
 *     by it and is drawn as their leaves, past their shadow range it casts
 *     so, and a forest reaches the horizon.
 *
 * The land is square: `extent` metres from the origin each way. Its
 * transform is ignored. It casts shadows (a hill's on the slope behind it,
 * on the grass and the plants) and receives them.
 *
 * THE SURFACE MODULE (scene_land_layer.glsl is the contract). GLSL that
 * defines
 *
 *     LandSurface landSurface(LandPoint q)
 *
 * where `q.p` is world (x, z), `q.height` the land's height there and
 * `q.normal` its normal (from the heights), returning
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
 * (0, 0, 1, 0), herbs 0). It may call `landHeight(vec2 p)` and
 * `landNoise(vec2 p, int seed)` (the height tree), `landData(int i)` (the
 * node's `data`, vec4 i of at most 64), its own functions and any other
 * tree's GLSL it carries (`someNode.glsl({ name: 'cover' })`). It runs
 * once a texel of a tile as the tile is made, not every frame, so it may
 * be as elaborate as the world wants. One that does not compile is logged
 * and the land draws nothing.
 *
 * Where a grass node's turf covers the land (`grass.shadeGround(land)`), the
 * turf shades it; the surface is what shows elsewhere.
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
 */

/**
 * @typedef {Object} LandOptions
 * @property {FastNoise} height - the land's height (m) at world (x, z): the
 *   tree run as `landNoise(p, seed)` on the GPU and by FastNoise2 on the CPU.
 *   Its nodes must be ones `glsl()` emits (docs/noise-api.js).
 * @property {number} [seed=0] - the seed the height runs with
 *   (`LAND_SEED` in the module).
 * @property {string|{source: string}} [surface] - GLSL defining
 *   `landSurface` (see above); default bare earth.
 * @property {ArrayLike<number>} [data] - what `landData(i)` reads: at most
 *   256 floats (64 vec4s).
 * @property {number} [extent=4096] - the land's half-size (m).
 * @property {number} [cellSize=0.5] - the finest ring's cell (m).
 * @property {number} [resolution=256] - a ring's cells a side (a multiple
 *   of 4): a cell is about 2 / resolution of the view's height on screen.
 * @property {number} [levels=10] - rings: they reach cellSize *
 *   resolution / 2 * 2^(levels - 1) metres.
 * @property {string} [name]
 * @property {boolean} [receivesShadow=true]
 * @property {boolean} [castsShadow=true]
 * @property {boolean} [visible=true]
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
   * The land's height at world (x, z), metres; NaN off the land.
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
