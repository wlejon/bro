// bro.flora — broflora ecosystem simulation
// =========================================
// Bindings for the broflora multi-scale plant simulation
// (Makowski et al. 2019, "Synthetic Silviculture"). A world is a ticking
// ecosystem: branch-module prototypes are registered up front, plants
// reference them, and `step(dt)` advances light → vigor → development →
// spawning → senescence. Geometry is emitted on demand as a bromesh
// Mesh plus parallel arrays of segments, foliage samples, and bloom
// anchors that downstream pipelines (leaf scatter, flower instancing,
// shader colour) consume.

/**
 * Create a world.
 *
 * @param {Object} [opts]
 * @param {number} [opts.rngSeed]
 *        Deterministic seed for the world rng. Defaults to broflora's
 *        internal golden-ratio constant.
 * @param {Object} [opts.climate]
 *        Annual climate. Defaults: tempBase 15 °C, precip 1000 mm/yr.
 * @param {number} [opts.climate.annualTempBase]
 * @param {number} [opts.climate.annualPrecip]
 * @param {number} [opts.climate.tempLapsePerUnit]
 *        Temperature lapse rate per metre of elevation (default -0.0065).
 * @param {Object} [opts.shadow]
 *        Uniform 3D shadow grid. Plants read Q_G from this grid.
 *        Required for non-trivial simulation; if omitted, the world
 *        defaults to no shadow grid which makes light Q = 0 everywhere.
 * @param {number[]} [opts.shadow.origin] World-space origin [x,y,z].
 * @param {number}   [opts.shadow.cellSize]
 * @param {number}   [opts.shadow.width]
 * @param {number}   [opts.shadow.height]
 * @param {number}   [opts.shadow.depth]
 * @param {number}   [opts.shadow.fill=1.0]  Initial Q_G value, full sun by default.
 * @returns {FloraWorld}
 */
bro.flora.createWorld;

// Example: minimal grove (one Y-shaped prototype, one seedling)
const world = bro.flora.createWorld({
  rngSeed: 0xC0FFEE,
  climate: { annualTempBase: 15, annualPrecip: 1000 },
  shadow:  { origin: [-8, 0, -8], cellSize: 1,
             width: 16, height: 16, depth: 16, fill: 1.0 },
});

/**
 * Register a branch-module prototype. Returns the prototype's index, to
 * be passed to addVoronoiSite / addPlant. Prototypes are immutable once
 * registered — every plant referencing a prototype holds a raw pointer
 * to broflora's internal storage, which the world manages.
 *
 * Topology rules: edges are undirected pairs into `nodes`, declared in
 * basipetal order (parent index < child index). `rootNode` is the
 * single node where the module attaches to its parent; `terminalNodes`
 * are the tip points where children attach.
 *
 * @typedef {Object} PrototypeSpec
 * @property {string}   [name]            Human-readable label for debugging.
 * @property {Array<{position:number[], ageAtBirth?:number, lengthMax?:number, thickening?:number}>} nodes
 * @property {Array<[number,number] | {a:number,b:number}>} edges
 * @property {number}   [rootNode=0]
 * @property {number[]} terminalNodes
 *
 * @param {PrototypeSpec} spec
 * @returns {number} prototype index, or -1 on failure
 */
world.addPrototype;

const protoY = world.addPrototype({
  name: 'Y',
  nodes: [
    { position: [ 0.0, 0.0, 0.0] },
    { position: [ 0.3, 1.0, 0.0], ageAtBirth: 0.2 },
    { position: [-0.3, 1.0, 0.0], ageAtBirth: 0.2 },
  ],
  edges: [[0, 1], [0, 2]],
  rootNode: 0,
  terminalNodes: [1, 2],
});

/**
 * Register a Voronoi site in (determinacy D, apicalControl λ) space.
 * Spawning picks the nearest site to the local (D', λ) of the parent
 * module. Two-plus sites are typical so module selection has somewhere
 * to drift toward as plants mature.
 *
 * @param {number} prototypeIndex  From addPrototype.
 * @param {number} determinacy     D coordinate.
 * @param {number} apicalControl   λ coordinate.
 */
world.addVoronoiSite;

world.addVoronoiSite(protoY, 0.2, 0.85);

/**
 * Plant a seedling. The plant's initial root module is created from
 * `prototypeIndex`; subsequent modules are spawned by the simulation.
 *
 * Species fields are partially applied — fields you omit keep their
 * defaults. See broflora's `Species` definition for the full list.
 *
 * @typedef {Object} PlantSpec
 * @property {number[]} origin                 World-space [x,y,z].
 * @property {number}   [age=0]                Initial plant age.
 * @property {Object}   [species]              Partial species override.
 * @property {number}   [prototypeIndex]       Root-module prototype.
 * @property {number}   [initialVigor]         Initial root vigor; defaults to species.minVigor*2.
 *
 * @param {PlantSpec} spec
 * @returns {number} plant index, or -1 on failure
 */
world.addPlant;

world.addPlant({
  origin: [0, 0, 0],
  species: { moduleMatureAge: 0.6, shadeTolerance: 0.5 },
  prototypeIndex: protoY,
});

/**
 * Advance the simulation by `dt`. Internally runs light → basipetal/
 * acropetal vigor → development (age + tropism) → spawning → senescence.
 *
 * @param {number} dt  Tick size.
 * @returns {FloraWorld}
 */
world.step;

for (let i = 0; i < 200; i++) world.step(0.1);

/**
 * Emit the world's plant geometry as a single Mesh (bromesh-compatible).
 * One faceted tapered cylinder per branch segment per module, with
 * `sides` ≥ 3.
 *
 * @param {number} [sides=6]
 * @returns {Mesh}
 */
world.emitMesh;

const mesh = world.emitMesh(6);
console.log('verts:', mesh.vertexCount, 'tris:', mesh.triangleCount);

/**
 * Emit the world's branch skeleton as a flat array, in lockstep with
 * emitFoliage(). Indices match: segs[i] / fol[i] describe the same
 * segment. Parent indices are absolute into the returned array, so a
 * single downstream leaf-scatter call can run over the whole world.
 *
 * @returns {Array<{from:number[], to:number[], radius:number, depth:number, parent:number}>}
 */
world.emitSegments;

/**
 * Per-segment foliage state, in lockstep with emitSegments(). Scalars
 * are in [0,1] (or [0,2] for age01) so they can drive vertex attributes
 * or scatter density without normalisation.
 *
 * @returns {Array<{mass:number, age01:number, vigor01:number, light01:number, senescence01:number, isTerminal:boolean}>}
 */
world.emitFoliage;

const segs = world.emitSegments();
const fol  = world.emitFoliage();
// Zip them: segs[i] and fol[i] describe the same prototype edge.

/**
 * Bloom / fruit anchor candidates — one per terminal node of each
 * terminal module on every flowering plant. Empty for pre-flowering
 * plants. Feed to bromesh.packAnchors to thin overlapping candidates,
 * then instance a flower mesh at each survivor.
 *
 * @returns {Array<{position:number[], normal:number[], age01:number, vigor01:number, senescence01:number}>}
 */
world.emitBloomAnchors;

const blooms = world.emitBloomAnchors();

/**
 * Validate world invariants (topological order, prototype refs,
 * Voronoi indices). Returns null on success, an error string on
 * failure. O(N) over modules — useful in tests, not every tick.
 *
 * @returns {string|null}
 */
world.validate;

// Read-only properties:
//   world.simTime         — total simulated time (sum of dt).
//   world.plantCount      — number of plants.
//   world.prototypeCount  — number of registered prototypes.
//   world.moduleCount     — sum of modules across all plants.
