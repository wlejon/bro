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

  /**
   * One merged Mesh of `leafMesh` stamped at the world's foliage samples
   * (bromesh scatterLeaves over the branch segments). `opts` are the leaf
   * placement options (density, scale, jitter, ...).
   *
   * @param {Object} leafMesh
   * @param {Object} [opts]
   * @returns {Object|null}
   */
  emitFoliageMesh(leafMesh, opts) {}

  /**
   * Foliage mesh for one plant, the per-plant form of emitFoliageMesh.
   *
   * @param {number} plantIdx
   * @param {Object} leafMesh
   * @param {Object} [opts]
   * @returns {Object|null}
   */
  emitPlantFoliageMesh(plantIdx, leafMesh, opts) {}

  /**
   * Blooms as geometry: `[petals, centers]`, one merged Mesh each. A petal
   * stamp (`petalMesh`, its +Y turned onto the anchor normal, scaled by the
   * anchor's age) at every flowering anchor, thinned to `opts.bloomCap` of
   * them (default 500) with anchors dimmer than `opts.bloomLightMin`
   * (default 0.18) skipped; a `centerMesh` stamp lifted along the normal
   * when one is given, else an empty centers Mesh.
   *
   * @param {Object} petalMesh
   * @param {Object|null} [centerMesh]
   * @param {Object} [opts]
   * @returns {Array<Object>}
   */
  emitBloomMesh(petalMesh, centerMesh, opts) {}

  /**
   * The world's branch segments packed for instanced tube rendering:
   * `{segments: Float32Array, segCount, boundsMin, boundsMax}`, eight
   * floats per segment (from xyz + from radius, to xyz + to radius);
   * segments thinner than `opts.minRadius` are left out.
   *
   * @param {Object} [opts]
   * @returns {Object}
   */
  emitBranchTubes(opts) {}

  /**
   * The world's branch segments packed for instanced foliage scatter:
   * `{segments: Float32Array, segCount, instSeg: Uint32Array,
   * instanceCount, boundsMin, boundsMax}` — the segment pack plus one
   * segment index per foliage instance, placed by the leaf placement
   * `opts`.
   *
   * @param {Object} [opts]
   * @returns {Object}
   */
  emitScatterSegments(opts) {}

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

/**
 * @param {number} strength
 * @param {number} [dirX=0]
 * @param {number} [dirY=0]
 */
bro.flora.setWind = function(strength, dirX, dirY) {};

/**
 * @param {number} strength
 * @param {number} [dirX=0]
 * @param {number} [dirY=0]
 */
bro.flora.wind = function(strength, dirX, dirY) {};

/**
 * @param {number} density
 */
bro.flora.setDensity = function(density) {};

/**
 * @param {number} density
 */
bro.flora.density = function(density) {};

/**
 * @param {number} dt
 */
bro.flora.update = function(dt) {};

bro.flora.clear = function() {};

/**
 * @param {Object} config
 */
bro.flora.placement = function(config) {};

/**
 * @param {Object} config
 */
bro.flora.addPlacement = function(config) {};

/**
 * @returns {Object}
 */
bro.flora.batches = function() {};

/**
 * @returns {Object}
 */
bro.flora.getBatches = function() {};

