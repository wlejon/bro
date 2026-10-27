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
 * and snow lines are its own GLSL of them. `q.footprint` is the texel's
 * width (m): its height is the noise's mean over that much ground, so a
 * line the surface draws from the height (snow, trees, rock) is a share
 * there, its threshold widened by the relief a texel that wide hides, or
 * a far range is flecked texel by texel. It returns
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
 * at the seed), `landNoise(vec2 p, int seed)`, `landGrid(int c, vec2 p)`
 * (the processes' channel c, zero with none) and `landGridCover(int level,
 * vec2 p)` (how much of landGrid's kept channels there are the regional
 * grids' of `level` or finer, 0 .. 1: 0 past the regions about the eye or
 * where the texel is as wide as the next coarser field's cells (a texel
 * wider than the level's own reads their mean over it), so a surface shading a
 * band of relief laid down on a level can spread its lines by the band
 * where the band is not there; see REGIONAL GRIDS) (on a round body each
 * takes the vec3 datum point `q.at` instead), `landData(int i)` (the
 * node's `data`, vec4 i of at most 64), its own functions and any other
 * tree's GLSL it carries (`someNode.glsl({ name: 'cover' })`). It runs
 * once a texel of a tile as the tile is made, not every frame, so it may
 * be as elaborate as the world wants. One that does not compile is logged
 * and the land draws nothing.
 *
 * Where a grass node's turf covers the land (`grass.shadeGround(land)`), the
 * turf shades it; the surface is what shows elsewhere.
 *
 * FILTERED READS (ground/land_filters.glsl). A texel stands for
 * `q.footprint` metres of ground, so every line the surface draws (snow,
 * trees, rock, wetness) must be the share of that ground past the line:
 * soft and the same every time, never a point's yes or no texel by texel,
 * and never the grid's cells. These give the surface its inputs that way,
 * each read at a scale clamped to at least the footprint, so what the
 * texel cannot hold is its mean and what that mean hides is its spread:
 *
 *     struct LandStat { float mean; float spread; }
 *         a value's mean over the footprint and its spread (standard
 *         deviation) there; landStatAdd(a, b) (independent: spreads in
 *         quadrature), landStatScale(a, k)
 *     float landShare(LandStat v, float t)
 *     float landShare(float mean, float spread, float t)
 *         the share of the footprint where v is over t (the normal
 *         distribution's, a step with no spread): "where is this line"
 *         in one call that blurs correctly at any footprint
 *     vec4 landGridMean(int c, P p, float w, out vec4 spread)
 *         channel c's mean over ground w (at least the texel) wide and its
 *         spread there: reads a quarter of w about p, each at half of w
 *         (the regional levels finer than that faded to the coarser field,
 *         each a texel's mean of the cells it spans: the levels as a
 *         pyramid)
 *     float landGridCell(P p)
 *         the cell (m) of the finest grid the land reads at p for the texel
 *         (the regional levels as far as each is in, the global grid's past
 *         them): a shape the grid's cells make (a D8 network's branches) is
 *         read at a cell or more
 *     LandTerrain landTerrain(LandPoint q, float scale, float base)
 *         the landform at `scale` (m): the land's heights a scale either way
 *         (each read at half the scale, the noise's octaves and the grids'
 *         levels finer than that at their mean), the scale at least 1.5
 *         texels, heights under `base` read as base (a sea's level). Its
 *         `slope` (rise along east, north; `grade` its length), `east` and
 *         `north` (the ground's frame, north toward the body's +y pole; on a
 *         flat body (1, 0, 0) and (0, 0, 1)), `hollow` (the ring's mean less
 *         the centre, m: > 0 a hollow or valley, < 0 a spur or crest, its
 *         relief over its surroundings -hollow; where the texel is wider
 *         than the scale, its mean over the texel: the wider read's times
 *         (scale / read)², so from orbit a hollow is the few metres a broad
 *         form makes), `height` (the heights' mean and spread: the relief
 *         there, less the slope's plane) and `scale` (the scale read)
 *     LandStat landGrain(vec3 P, float seed, float wavelength, int octaves,
 *                        float lacunarity, float gain, float scale)
 *         a fractal value noise of body point P (`landBodyPoint(q.at)` or
 *         `(q.p)`): each octave faded to its mean as the footprint (or
 *         `scale`, where wider) passes 0.3 to 0.6 of its wavelength; mean
 *         the held octaves (all of them together a spread of about 1),
 *         spread the share the footprint does not hold, so a line it ragged
 *         is crisp to the texel where the texel holds it and the share of
 *         each side where it does not; `landValueNoise(vec3)` its octave
 *
 * (P is vec3, the datum point q.at, on a round body; vec2, q.p, on a flat
 * one.) The grids themselves (landGrid) are read through the cubic
 * B-spline (below), so slopes and curvatures taken of a channel, or of
 * heights built on one, show no cells.
 *
 * @example
 *   // A tree line ragged by a grain, cut where the view holds it and the
 *   // share of each side where it does not, at any footprint.
 *   LandTerrain t = landTerrain(q, 90.0, 0.0);
 *   LandStat grain = landGrain(landBodyPoint(q.at), 5.3, 1600.0, 8, 0.47, 0.68, 0.0);
 *   float temp = 12.0 - 6.5 * q.height / 1000.0 + 0.02 * t.hollow;          // a gully the milder
 *   LandStat line = LandStat(temp + 2.0 * grain.mean,
 *                            length(vec2(2.0 * grain.spread, 6.5 * t.height.spread / 1000.0)));
 *   float trees = landShare(line, -3.0);                                    // forest where over -3 C
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
 * of the tree's detail rides on it in y (none in a sea, more on a range).
 * Its z and w are the world's own, read back with it: the plants standing on
 * the land read them in their rules (`{ own: 'z' | 'w' }`, vegetation-api.js),
 * so a climate worked out here places the forests as it colours the ground. Every
 * channel is the surface module's through `landGrid(c, p)` (moisture, flow,
 * rock: whatever the world computes). They run on the GPU when the land is
 * made (a new height, seed, extent, body or processes runs them again),
 * 128 iterations a frame, then channel 0 is read back once: until then the
 * land draws nothing, its heights are NaN and `landStats().ready` is false.
 * Processes that do not compile are logged and the land stays unready.
 * Between cell centres both sides read the grid through the cubic B-spline
 * over the four centres about a point each way, the same way (C2: a
 * channel's slopes and curvatures, and those of the heights built on it, are
 * continuous across the cells; it smooths, reading (a + 4 b + c) / 6 at a
 * centre); on a round body the faces about a point within a cell of an edge
 * are cross-faded. A texel wider than a grid's cells reads their mean over
 * it.
 *
 * REGIONAL GRIDS (`processes.regions`; scene/world/land_regions.h,
 * ground/land_region_layer.glsl). Finer grids of the same processes made
 * under the eye as it moves and dropped behind it, so a shape the world's
 * kernels make (a valley a stream cut, a drained ridgeline) reaches down to
 * tens of metres. `levels` levels, level 0's cells `cell` chart metres and
 * each coarser level's `ratio` times its finer's; a level is a lattice of
 * square regions of `cells` texels a side, overlapping their neighbours by
 * an `apron` of texels each side (cross-faded over two aprons' widths, so a
 * level's regions sum to one: no seams), on a round body running on past a
 * cube face's edge (the faces cross-faded too). The regions whose ground
 * comes within `reach` lattice steps (half a step at reach 0) of the ground
 * under the eye are wanted at every level whose cell is no finer than a
 * pixel from the eye's height: a disc, crossing a cube face's edges and
 * corners; coarser levels are made first. A level is read only within the
 * disc about the eye that its regions in cover whole (out to the nearest
 * wanted region not yet in, or the wanted disc's edge), fading into the
 * coarser field from 0.6 to 0.9 of that radius, so its edge is a circle,
 * never a region's straight side; as regions arrive the disc grows, eased
 * over some frames. Each region runs `init` (the source's function) once over its
 * cells, then `passes` as the global grid's run, with the same kernel API
 * (procRead's offsets past the region's edge read its edge cells) and:
 *
 *     int procLevel()               the region's level (0 the finest; -1 on the global grid)
 *     int procBorder()              cells to the region's nearest edge (0 on it; huge on the global grid)
 *     vec4 procCoarse(int c)        channel c of the next coarser field at the cell: the
 *                                   coarser levels blended over the global grid (on the
 *                                   global grid, its own cell)
 *     float procCoarseCellSize()    that field's cell, chart metres
 *
 * so a region starts from the coarser field and takes its boundary from it:
 * flow entering at its edges is the coarser field's (a kernel holds its
 * border cells to procCoarse, or routes into them). Where a level's regions
 * are in, the land reads them, the coarser field making up what they do not
 * cover (a level's edge, regions still to come), level by level down from
 * the global grid; the first `keep` channels are the regions' and the rest
 * the global grid's. A region refines the coarser field and never replaces
 * it: the components it `conserve`s (the ground's height) keep the coarser
 * field's mean over each of its cells, whatever the passes did, so a region
 * coming in moves the ground by its detail, never by a drift. The tiles, landGrid and the surface read it; the CPU's
 * heights read the same regions read back, blended the same way. The engine
 * streams them up to four of a level at a time, a frame's budget of steps
 * at a time (a band of init rows, a whole pass, or the finish, each over
 * every region of the batch; each region's result is its own, the same as
 * made alone), so a region in arrives over some frames with no hitch, and
 * the tiles under it that read it (within its level's disc) are made again.
 * `landStats()` says how many are in and how many wanted are pending.
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
 * @property {number} [roughness=0.08] - the surface's roughness where its
 *   waves are a pixel or more; where they are far under one (a pixel tens
 *   of metres) it widens to the spread of their slopes (0.5), so the sun's
 *   glint seen from afar is a broad soft patch.
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
 * @property {LandRegions} [regions] - finer grids of these processes under
 *   the eye (see REGIONAL GRIDS above); none without.
 */

/**
 * @typedef {Object} LandRegions
 * @property {string} init - the source's function making a region's first
 *   values (`void init(inout vec4 v[PROC_CHANNELS])`; v starts at 0, v[0].y
 *   at 1, as procInit's), typically from `procCoarse`.
 * @property {Array<{fn: string, iterations?: number}>} [passes] - run in
 *   order after init on each region, each `iterations` times (default 1).
 * @property {number} [cells=512] - texels a region's side (at most 1024).
 * @property {number} [apron=32] - texels past a region's core each side,
 *   overlapping its neighbours'.
 * @property {number} [cell=64] - level 0's cell, chart metres.
 * @property {number} [ratio=4] - each level's cell over its finer's.
 * @property {number} [levels=3] - levels, at most 6.
 * @property {number} [reach=1] - the wanted disc's radius about the ground
 *   under the eye, in lattice steps (`cells - 2 apron` cells; 0: half a step).
 * @property {number} [keep=2] - channels the regions keep and are read for
 *   (the first `keep`; the others are the global grid's).
 * @property {string[]} [conserve=['0.x']] - the kept channels' components a
 *   region refines and never replaces ('c.x' .. 'c.w'): once its passes are
 *   done, the engine takes out their drift from the coarser field (their
 *   difference's mean under a tent as wide as the coarser field's cell), so
 *   over each coarser cell their mean is the coarser field's and the region
 *   adds detail only. The ground's height by default; a flow, an area or a
 *   mask is not a mean and is left out. [] conserves nothing.
 * @example
 *   // The global grid's continents eroded again at 64 m cells under the eye.
 *   processes: { source, resolution: 512, channels: 4, passes: [...],
 *     regions: { init: 'regionInit', cells: 512, apron: 32, cell: 64, ratio: 4, levels: 3,
 *                passes: [{ fn: 'regionRoute', iterations: 320 }, { fn: 'regionErode', iterations: 384 }] } }
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
 * @property {number} regions - regional grids in (made and read back).
 * @property {number} regionsPending - regions wanted and not yet in.
 * @property {boolean} groundReady - the regions about the ground under the
 *   eye are in, and every wanted level is read whole there (its disc grown
 *   past it).
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
   * under (x, z), the body's curve included. With `processes.regions` the
   * answer is the regions' where they are in, blended over the global
   * grid's, so it moves as they come in about the eye: a thing placed on
   * the ground before `landStats().groundReady` (a camera held some metres
   * over it, a prop) is placed again once it is, or it floats or sinks by
   * what the regions added; how much depends on when it was placed, which
   * in a window, whose frames never wait for a compile, varies run to run.
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
