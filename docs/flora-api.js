// ── Classes & Interfaces ─────────────────────────────────────────────────────

class FloraWorld {

  /**
   * Current simulation time in seconds.
   * @readonly
   * @type {number}
   */
  simTime;

  /**
   * Total number of living plants in the world.
   * @readonly
   * @type {number}
   */
  plantCount;

  /**
   * Total number of registered module prototypes.
   * @readonly
   * @type {number}
   */
  prototypeCount;

  /**
   * Total active module instances across all plants.
   * @readonly
   * @type {number}
   */
  moduleCount;

  /**
   * Register a branch module prototype into the world state.
   *
   * @param {Object} spec
   * @returns {number}
   */
  addPrototype(spec) {}

  /**
   * Add a voronoi site defining spatial dominance and archetype distribution.
   *
   * @param {number} prototypeIndex
   * @param {number} [determinacy=1]
   * @param {number} [apicalControl=0.5]
   * @returns {FloraWorld}
   */
  addVoronoiSite(prototypeIndex, determinacy, apicalControl) {}

  /**
   * Add a plant specimen into the ecosystem.
   *
   * @param {Object} spec
   * @returns {number}
   */
  addPlant(spec) {}

  /**
   * Remove a plant specimen by index (swap-and-pop).
   *
   * @param {number} plantIdx
   * @returns {boolean}
   */
  removePlant(plantIdx) {}

  /**
   * Advance the simulation by delta time.
   *
   * @param {number} dt
   * @returns {FloraWorld}
   */
  step(dt) {}

  /**
   * Query full runtime and species snapshot for a plant.
   *
   * @param {number} plantIdx
   * @returns {Object|null}
   */
  plantInfo(plantIdx) {}

  /**
   * Update climate temperature and precipitation parameters in real-time.
   *
   * @param {Object} opts
   * @returns {FloraWorld}
   */
  setClimate(opts) {}

  /**
   * Sample the world shadow grid at a world-space coordinate [x, y, z].
   *
   * @param {Array<number>} pos
   * @returns {number|null}
   */
  sampleShadow(pos) {}

  /**
   * Verify integrity of world module hierarchy.
   * @returns {string|null}
   */
  validate() {}

  /**
   * Emit procedural mesh geometry for all living plant branches.
   *
   * @param {number} [sides=6]
   * @returns {Object}
   */
  emitMesh(sides) {}

  /**
   * Emit linear branch segments with radii and parent indices.
   * @returns {Array<Object>}
   */
  emitSegments() {}

  /**
   * Emit foliage particle points.
   * @returns {Array<Object>}
   */
  emitFoliage() {}

  /**
   * Emit blossom and flowering anchor points.
   * @returns {Array<Object>}
   */
  emitBloomAnchors() {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.flora — ecosystem simulation (Synthetic Silviculture)
 * =============================================================================
 *
 * Procedural plant and ecosystem growth simulation based on Makowski et al. (2019).
 * Simulates bud fate, developmental archetypes, hydraulic architecture, and competitive
 * light interception across multiple species in a shared voxel shadow grid.
 * @example
 * const world = bro.flora.createWorld({ rngSeed: 42 });
 *   const protoIdx = world.addPrototype(bro.flora.prototypes.monopodial(3, 0.6));
 *   const plantIdx = world.addPlant({
 *     origin: [0, 0, 0],
 *     prototypeIndex: protoIdx,
 *     species: { maxAge: 50.0, apicalControl: 0.8 }
 *   });
 *   world.step(1.0);
 *   const mesh = world.emitMesh(6);
 */
/**
 * Instantiate an ecosystem simulation world.
 *
 * @param {Object} [opts] - Simulation initialization options (rngSeed, climate, shadow grid).
 * @returns {FloraWorld} World instance handle.
 */
bro.flora.createWorld = function(opts) {};

/**
 * Procedural leaf cluster geometry generator.
 *
 * @param {*} phyllotaxy - Phyllotactic arrangement type.
 * @param {Object} [opts] - Leaf cluster configuration options.
 * @returns {Object} Mesh instance containing generated leaf vertices and indices.
 */
bro.flora.leafCluster = function(phyllotaxy, opts) {};

