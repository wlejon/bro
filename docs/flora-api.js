/**
 * bro.flora, ecosystem simulation (broflora sibling)
 *
 * Procedural plant and ecosystem growth after Makowski et al. 2019,
 * "Synthetic Silviculture": plants grow as trees of branch *modules*, each an
 * instance of a module prototype (a small node/edge graph). Bud fate,
 * developmental archetypes, pipe-model thickening, tropism and competitive
 * light interception in a shared voxel shadow grid all run inside
 * `world.step(dt)`. The emit methods turn the current state into geometry:
 * bromesh `Mesh`es, branch segment lists, per-segment foliage state, bloom
 * anchors, and packed instance buffers.
 *
 * `bro.flora.available` is `true` whenever broflora is compiled in.
 *
 * ── Minimal world ──
 *
 *   const world = bro.flora.createWorld({ rngSeed: 42 });
 *   const proto = world.addPrototype(bro.flora.prototypes.monopodial(3, 0.6));
 *   world.addVoronoiSite(proto);               // new modules need a site to pick from
 *   const idx = world.addPlant({
 *     origin: [0, 0, 0],
 *     prototypeIndex: proto,
 *     species: { maxAge: 50, apicalControl: 0.8 },
 *   });
 *   for (let i = 0; i < 20; i++) world.step(1.0);
 *   const bark   = world.emitMesh(6);                          // Mesh
 *   const leaves = world.emitFoliageMesh(bro.flora.leafCluster('opposite'));
 *
 * ── Prototype selection ──
 * A plant's root module uses the prototype named by `addPlant`'s
 * `prototypeIndex`. Every module spawned after that (and every seedling a
 * mature plant drops) picks its prototype from the world's Voronoi sites:
 * the site nearest to the module's current (determinacy, apicalControl)
 * point wins. A world with no sites grows each plant's root module only.
 *
 * ── Wind and density ──
 * `bro.flora.setWind` / `setDensity` / `update` are process-global, not per
 * world. The global wind bends geometry *at emit time* in emitMesh,
 * emitPlantMesh, emitFoliageMesh, emitPlantFoliageMesh,
 * emitFoliageTransforms and emitSegmentTransforms, and `update(dt)` sways
 * the placement batches with the same model: the sway is zero at y = 0 and
 * grows with height (0.04h + 0.015h^2), a gust wave phased by ground
 * position moves neighbours out of step, and each point is offset
 * downwind and tilted rigidly toward the wind (at most 0.35 rad), so a
 * vertex, its normal and an instance matrix at the same point move alike
 * and matrices stay orthonormal. `update(dt)` advances the gust phase.
 * emitSegments, emitBranchTubes, emitScatterSegments, emitBloomMesh and
 * the SDF meshes are never bent.
 * The global density (default 1) multiplies the leaf placement
 * `perUnitLength` of every foliage emit.
 *
 * ── Instance matrices ──
 * emitFoliageTransforms, emitSegmentTransforms and the placement batches
 * use 16 floats per instance laid out row by row: floats 0-2 / 4-6 / 8-10
 * are the three rows of the 3x3 basis (columns = local X, Y, Z axes, scale
 * baked in) and floats 3 / 7 / 11 are the translation. Floats 12-15 are
 * not a fourth matrix row: they are the RGBA instance tint of bro's
 * InstancedMeshNode layout, written as white (1, 1, 1, 1). Read the buffer
 * as a row-major 3x4 affine plus a tint, never as a column-major 4x4.
 *
 * ── Foliage density ──
 * Every foliage emit places leaves with bromesh's leaf placement
 * (LeafPlacementOptions below). When `opts.densityWeight` is not given, it
 * is filled per segment from the simulation:
 *   (0.12 + 0.88 * lightExposure01) * min(1, age01) * (1 - senescence01)
 *     * twigGrade01^2 * (isTerminal ? 1 : 0.1)
 * so shaded, young, dying or thick segments go bare.
 */


// ═════════════════════════════════════════════════════════════════════════════
// Option and result shapes
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @typedef {Object} FloraWorldOptions
 * @property {number} [rngSeed] - Seed for the world's RNG (seeding, variation).
 * @property {FloraClimate} [climate]
 * @property {Object} [shadow] - The voxel shadow grid plants compete in. Omit
 *   it and there is no grid: every module sees full sun and sampleShadow()
 *   returns null.
 * @property {Array<number>} [shadow.origin=[0,0,0]] - World position of cell (0,0,0)'s min corner.
 * @property {number} [shadow.cellSize=1]
 * @property {number} [shadow.width=0] - Cells along +X.
 * @property {number} [shadow.height=0] - Cells along +Y (vertical).
 * @property {number} [shadow.depth=0] - Cells along +Z.
 * @property {number} [shadow.fill=1] - Initial light value of every cell (1 = full sun).
 */

/**
 * @typedef {Object} FloraClimate
 * @property {number} [annualTempBase=15] - Temperature at elevation 0, degrees C.
 * @property {number} [annualPrecip=1000] - Annual precipitation, mm/yr.
 * @property {number} [tempLapsePerUnit=-0.0065] - Temperature change per unit of height.
 */

/**
 * A branch module prototype: a small graph that grows as one unit. The
 * `bro.flora.prototypes.*` factories return this shape, and addPrototype
 * accepts it.
 * @typedef {Object} FloraPrototypeSpec
 * @property {string} [name]
 * @property {Array<{position: Array<number>, ageAtBirth?: number, lengthMax?: number, thickening?: number}>} nodes -
 *   Node rest positions in module space. `ageAtBirth` (default 0) is the
 *   module age at which the node appears, `lengthMax` (default 1) caps the
 *   incoming segment's length, `thickening` (default 1) is its length growth
 *   rate. At least one node is required.
 * @property {Array<Array<number>|{a: number, b: number}>} [edges] - Node index
 *   pairs, `[a, b]` or `{a, b}`, parent before child.
 * @property {number} [rootNode=0]
 * @property {Array<number>} [terminalNodes] - Nodes where child modules (and blooms) attach.
 */

/**
 * Species parameters. addPlant takes any subset; unnamed fields keep these
 * defaults. plantInfo returns the full set.
 * @typedef {Object} FloraSpecies
 * @property {number} [maxVigor=1] @property {number} [minVigor=0.01] @property {number} [rootVigorMax=1]
 * @property {number} [apicalControl=0.55] - Main vs lateral vigor split (juvenile).
 * @property {number} [determinacy=0.5] - Juvenile determinacy (prototype-selection axis).
 * @property {number} [shadeTolerance=0.3]
 * @property {number} [apicalControlMature=0.85] @property {number} [determinacyMature=0.2]
 * @property {Array<number>} [tropismDir=[0,-1,0]] @property {number} [tropismG1=1] @property {number} [tropismG2=1]
 * @property {number} [growthScale=1]
 * @property {number} [climateOptT=15] @property {number} [climateOptP=1000]
 * @property {number} [climateSigT=10] @property {number} [climateSigP=500]
 * @property {number} [maxAge=100] @property {number} [floweringAge=10] @property {number} [seedingRadius=5]
 * @property {number} [moduleMatureAge=1] @property {number} [pipeExp=2.5] @property {number} [leafDiameter=0.02]
 * @property {number} [terrainAnchorWeight=0] @property {number} [maxSeedingSlope=1.5707963]
 * @property {number} [distributionWeightCollisions=1] @property {number} [distributionWeightTropism=0.5]
 * @property {number} [orthotropy=0.3] @property {number} [individualVariation=0]
 */

/**
 * @typedef {Object} FloraPlantSpec
 * @property {Array<number>} [origin=[0,0,0]]
 * @property {number} [age=0] - Starting plant age.
 * @property {number} [prototypeIndex] - Prototype of the root module. Omit it
 *   and the plant starts with no modules (and emits nothing).
 * @property {number} [initialVigor] - Root module vigor; default `species.minVigor * 2`.
 * @property {FloraSpecies} [species]
 */

/**
 * One branch segment. Segments come per prototype edge per module, in
 * module-topological order; `parent` indexes into the same returned array
 * (world-level lists keep counting across plants) or is -1 for a root.
 * @typedef {Object} FloraSegment
 * @property {Array<number>} from - World-space start [x, y, z].
 * @property {Array<number>} to - World-space end.
 * @property {number} radius - Radius at the `to` end.
 * @property {number} depth - Hops from the plant's root segment (roots are 0).
 * @property {number} parent - Parent segment index, or -1.
 */

/**
 * Per-segment foliage state, index-aligned with the matching segment list.
 * @typedef {Object} FloraFoliageSample
 * @property {number} mass - Default leaf density multiplier in [0, 1].
 * @property {number} age01 - module age / moduleMatureAge, clamped to [0, 2].
 * @property {number} vigor01 - module vigor / maxVigor, [0, 1].
 * @property {number} light01 - Effective light after the shade-tolerance floor.
 * @property {number} lightExposure01 - Raw illumination, [0, 1] (true shadow gradient).
 * @property {number} senescence01 - 0 until maxAge, ramps to 1 over the next 20% of maxAge.
 * @property {boolean} isTerminal - The owning module has no child modules.
 * @property {number} twigGrade01 - 1 at leaf thickness, 0 on branches over ~6x leafDiameter.
 */

/**
 * A flower/fruit candidate at a terminal node of a terminal module. Only
 * flowering plants (age past `floweringAge`) produce anchors.
 * @typedef {Object} FloraBloomAnchor
 * @property {Array<number>} position - World-space [x, y, z] of the twig tip.
 * @property {Array<number>} normal - Unit direction along the twig, outward (+Y if degenerate).
 * @property {number} age01 @property {number} vigor01 @property {number} lightExposure01 @property {number} senescence01
 */

/**
 * bromesh leaf placement options, read by every foliage emit. Unnamed
 * fields keep these defaults.
 * @typedef {Object} FloraLeafPlacementOptions
 * @property {number} [maxRadius=0.05] - Skip segments thicker than this (keeps leaves off the trunk).
 * @property {number} [minDepth=1] - Skip segments shallower than this.
 * @property {boolean} [terminalOnly=false] - Only segments with no child segments.
 * @property {number} [perUnitLength=20] - Average leaves per unit length (times the global density).
 * @property {number} [densityFalloff=0] - >0 biases leaves toward segment tips.
 * @property {Array<number>} [densityWeight] - Per-segment multiplier; default computed from the simulation (see top).
 * @property {number} [upBias=0.5] - 0 = leaves point radially out, 1 = toward +Y.
 * @property {number} [tiltJitter=0.3] - Radians of random pitch.
 * @property {number} [rollJitter=0.2] - Radians of random roll.
 * @property {number} [baseScale=1] @property {number} [scaleJitter=0.2] - Fraction of baseScale.
 * @property {number} [scaleByRadius=0] - 1 = scale leaves with radius / maxRadius.
 * @property {number} [dedupRadius=0] - Minimum spacing between leaf origins; 0 = off.
 * @property {number} [seed=0]
 */

/**
 * @typedef {Object} FloraSdfMeshOptions
 * @property {number} [voxelSize=0.04] - Grid spacing; values <= 0.005 fall back
 *   to 0.04. Each axis is clamped to 8..256 voxels, so a large plant gets a
 *   coarser grid than asked for.
 * @property {number} [smoothK=0.03] - Smooth-union blend radius; <= 1e-4 is a hard union.
 * @property {boolean} [useSurfaceNets=true] - false = marching cubes.
 * @property {number} [margin=0.08] - Padding added around the bounds (on top of smoothK).
 */


// ═════════════════════════════════════════════════════════════════════════════
// FloraWorld
// ═════════════════════════════════════════════════════════════════════════════

/**
 * A simulation world. Not constructible (`new bro.flora.FloraWorld()`
 * throws); get one from `bro.flora.createWorld`. Also exposed as the global
 * `FloraWorld` for `instanceof`.
 *
 * Methods taking a `plantIdx` return null (mesh emits) or `[]` (list emits)
 * for an index out of range.
 */
class FloraWorld {

  /** Simulation time in seconds. @readonly @type {number} */
  simTime;

  /** Number of plants (seeding during step() can add more). @readonly @type {number} */
  plantCount;

  /** Number of registered prototypes. @readonly @type {number} */
  prototypeCount;

  /** Module instances across all plants. @readonly @type {number} */
  moduleCount;

  /**
   * Register a module prototype. Prototypes must be registered before any
   * plant or site refers to them.
   * @param {FloraPrototypeSpec} spec
   * @returns {number} The prototype index, or -1 if `spec` has no nodes.
   */
  addPrototype(spec) {}

  /**
   * Add a Voronoi site in the (determinacy, apicalControl) plane. Every new
   * module takes the prototype of the site nearest its own (determinacy,
   * apical control) point, so several sites let one plant switch
   * architecture as it matures. Not a spatial position.
   * @param {number} prototypeIndex
   * @param {number} [determinacy=1]
   * @param {number} [apicalControl=0.5]
   * @returns {FloraWorld} this
   */
  addVoronoiSite(prototypeIndex, determinacy, apicalControl) {}

  /**
   * Add a plant.
   * @param {FloraPlantSpec} spec
   * @returns {number} The plant index, or -1 when `spec` is not an object or
   *   `prototypeIndex` names no prototype.
   */
  addPlant(spec) {}

  /**
   * Remove a plant by swap-and-pop: the last plant takes index `plantIdx`.
   * @param {number} plantIdx
   * @returns {boolean} false for an index out of range.
   */
  removePlant(plantIdx) {}

  /**
   * Advance the simulation by `dt` seconds (throws without `dt`).
   * @param {number} dt
   * @returns {FloraWorld} this
   */
  step(dt) {}

  /**
   * Runtime snapshot of one plant: `{origin, age, flowering, senescing,
   * moduleCount, effectiveRootVigorMax, rootVigor?, rootLight?, species}`,
   * where `rootVigor` / `rootLight` exist only when the plant has modules and
   * `species` is the full FloraSpecies.
   * @param {number} plantIdx
   * @returns {Object|null} null for an index out of range.
   */
  plantInfo(plantIdx) {}

  /**
   * Replace climate fields; unnamed fields keep their value.
   * @param {FloraClimate} opts
   * @returns {FloraWorld} this
   */
  setClimate(opts) {}

  /**
   * Light value of the shadow-grid cell containing `pos` (1 = full sun).
   * @param {Array<number>} pos - [x, y, z]
   * @returns {number|null} null when there is no grid or `pos` is outside it.
   */
  sampleShadow(pos) {}

  /**
   * Check the world's structural invariants.
   * @returns {string|null} null when valid, else the first error.
   */
  validate() {}

  // ── Branch geometry ──

  /**
   * Bark mesh of every plant: one tapered open cylinder per segment,
   * `sides` facets around. Wind-bent.
   * @param {number} [sides=6] - An integer in [3, 256]: TypeError for a
   *   non-number, RangeError for NaN, a fraction or a value out of range.
   * @returns {Mesh}
   */
  emitMesh(sides) {}

  /**
   * Bark mesh of one plant. Wind-bent.
   * @param {number} plantIdx
   * @param {number} [sides=6] - An integer in [3, 256], as for emitMesh.
   * @returns {Mesh|null}
   */
  emitPlantMesh(plantIdx, sides) {}

  /**
   * Every plant's branch segments, parents indexed across the whole list.
   * @returns {Array<FloraSegment>}
   */
  emitSegments() {}

  /**
   * One plant's branch segments.
   * @param {number} plantIdx
   * @returns {Array<FloraSegment>}
   */
  emitPlantSegments(plantIdx) {}

  /**
   * One instance matrix per world segment (see "Instance matrices"), mapping
   * a unit cylinder along local +Z (0..1) with radius 1 onto the segment:
   * X/Y axes scaled by the segment radius (min 0.001), Z by its length,
   * origin at `from`. Wind-bent.
   * @returns {Float32Array} 16 floats per segment; empty with no segments.
   */
  emitSegmentTransforms() {}

  /**
   * World segments packed for instanced tube rendering: `{segments,
   * segCount, boundsMin, boundsMax}`. `segments` is a Float32Array of 8
   * floats per segment: from xyz, from radius (the parent segment's radius,
   * so tubes taper), to xyz, to radius. Zero-length segments and ones with
   * radius below `opts.minRadius` are dropped. Bounds are [x, y, z] arrays
   * padded by the largest radius ([0,0,0] when empty).
   * @param {{minRadius?: number}} [opts] - minRadius defaults to 0.
   * @returns {{segments: Float32Array, segCount: number, boundsMin: Array<number>, boundsMax: Array<number>}}
   */
  emitBranchTubes(opts) {}

  /**
   * Watertight organic mesh of one plant: every segment becomes a capsule
   * (radius = its tip radius), smooth-unioned in a bromesh SDF graph and
   * meshed. Not wind-bent. An empty plant gives an empty Mesh.
   * @param {number} plantIdx
   * @param {FloraSdfMeshOptions} [opts]
   * @returns {Mesh|null}
   */
  emitPlantSdfMesh(plantIdx, opts) {}

  /**
   * The SDF mesh of every plant in the world, as one Mesh.
   * @param {FloraSdfMeshOptions} [opts]
   * @returns {Mesh}
   */
  emitWorldSdfMesh(opts) {}

  // ── Foliage ──

  /**
   * Foliage state per world segment, index-aligned with emitSegments().
   * @returns {Array<FloraFoliageSample>}
   */
  emitFoliage() {}

  /**
   * Foliage state per segment of one plant, aligned with emitPlantSegments().
   * @param {number} plantIdx
   * @returns {Array<FloraFoliageSample>}
   */
  emitPlantFoliage(plantIdx) {}

  /**
   * Leaf instance matrices for the whole world (see "Instance matrices"),
   * placed by `opts`. Each maps a leaf in `leafCard` space (+Z tip, +Y card
   * normal, +X side) to world space. Wind-bent.
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Float32Array} 16 floats per leaf; empty when nothing is placed.
   */
  emitFoliageTransforms(opts) {}

  /**
   * One merged Mesh of `leafMesh` stamped at every leaf placement over the
   * world's segments. `leafMesh` is a Mesh or any `{positions, normals?,
   * uvs?, colors?, indices}` typed-array object. Wind-bent.
   * @param {Mesh|Object} leafMesh
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Mesh|null} null when `leafMesh` is missing or empty; an empty
   *   Mesh when the world has no segments.
   */
  emitFoliageMesh(leafMesh, opts) {}

  /**
   * emitFoliageMesh for one plant.
   * @param {number} plantIdx
   * @param {Mesh|Object} leafMesh
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {Mesh|null}
   */
  emitPlantFoliageMesh(plantIdx, leafMesh, opts) {}

  /**
   * World segments packed for GPU foliage scatter: `{segments, segCount,
   * instSeg, instanceCount, boundsMin, boundsMax}`. `segments` holds 8 floats
   * per kept segment: from xyz, radius, then the *direction* `to - from`
   * xyz, then 0. `instSeg` is a Float32Array with one entry per leaf
   * instance, the index (as a float) of its segment in `segments`. The leaf
   * count per segment is `floor(length * perUnitLength * weight + hash)`,
   * capped at 4096, with the filters of `opts` (maxRadius, minDepth,
   * terminalOnly, densityWeight) applied; the jitter / scale fields are left
   * to the shader. Bounds are unpadded ([0,0,0] when empty).
   * @param {FloraLeafPlacementOptions} [opts]
   * @returns {{segments: Float32Array, segCount: number, instSeg: Float32Array, instanceCount: number, boundsMin: Array<number>, boundsMax: Array<number>}}
   */
  emitScatterSegments(opts) {}

  // ── Blooms ──

  /**
   * Bloom anchors of every flowering plant.
   * @returns {Array<FloraBloomAnchor>}
   */
  emitBloomAnchors() {}

  /**
   * Bloom anchors of one plant (empty until it flowers).
   * @param {number} plantIdx
   * @returns {Array<FloraBloomAnchor>}
   */
  emitPlantBloomAnchors(plantIdx) {}

  /**
   * Blooms as geometry: `[petals, centers]`, one merged Mesh each. The
   * anchors are first strided down to at most `opts.bloomCap`, then anchors
   * with `lightExposure01` below `opts.bloomLightMin` are skipped, so the
   * result can hold fewer than bloomCap. At each survivor `petalMesh` is
   * stamped with its +Y turned onto the anchor normal and scaled by
   * `0.8 + 0.5 * min(1, age01)`; `centerMesh`, when given, is stamped the
   * same way lifted `0.012 * scale` along the normal. Without `centerMesh`
   * the centers Mesh is empty. Not wind-bent.
   * @param {Mesh|Object} petalMesh - Required (throws when undefined).
   * @param {Mesh|Object|null} [centerMesh]
   * @param {{bloomCap?: number, bloomLightMin?: number}} [opts] - bloomCap
   *   defaults to 500 (0 means 1), bloomLightMin to 0.18.
   * @returns {Array<Mesh>|null} null when `petalMesh` is empty.
   */
  emitBloomMesh(petalMesh, centerMesh, opts) {}
}


// ═════════════════════════════════════════════════════════════════════════════
// bro.flora
// ═════════════════════════════════════════════════════════════════════════════

/**
 * Create a simulation world.
 * @example
 *  const world = bro.flora.createWorld({
 *    rngSeed: 7,
 *    climate: { annualTempBase: 12 },
 *    shadow: { origin: [-10, 0, -10], cellSize: 0.5, width: 40, height: 40, depth: 40 },
 *  });
 * @param {FloraWorldOptions} [opts]
 * @returns {FloraWorld}
 */
bro.flora.createWorld = function(opts) {};

/** @type {typeof FloraWorld} Not constructible; for `instanceof`. */
bro.flora.FloraWorld = FloraWorld;

/**
 * Leaf arrangement enum. Also exposed as `bro.flora.phyllotaxy`, and each
 * value has a lowercase alias (`alternate` ... `compoundPinnate`).
 * @enum {number}
 */
bro.flora.Phyllotaxy = {
  /** Two-ranked, alternating sides (oak, elm, birch). */
  Alternate: 0,
  /** Decussate pairs with a 90 degree twist per node (maple, ash). */
  Opposite: 1,
  /** Golden-angle rosette (magnolia, apple). */
  Spiral: 2,
  /** Needle bundle from a basal sheath (pine). */
  Fascicle: 3,
  /** Leaflet pairs along a rachis plus a terminal leaflet (walnut, rowan). */
  CompoundPinnate: 4,
};

/**
 * Module prototype factories. Each returns a FloraPrototypeSpec to pass to
 * `world.addPrototype` (or to edit first).
 */
bro.flora.prototypes = {
  /** One straight segment, one terminal: the unbranched "I" pole. @returns {FloraPrototypeSpec} */
  straight() {},
  /** A symmetric planar two-terminal "Y" fork. @returns {FloraPrototypeSpec} */
  fork() {},
  /** A short trunk topped by `arms` (clamped 2-8) terminals spread around +Y; `spread` (0..1) leans them out. The workhorse for rounded crowns. @param {number} [arms=3] @param {number} [spread=0.55] @returns {FloraPrototypeSpec} */
  whorl(arms, spread) {},
  /** A dominant upward leader (terminal 0) with `lateralBranches` (1-4) side arms: excurrent / conifer growth. @param {number} [lateralBranches=2] @param {number} [lateralSpread=0.7] @returns {FloraPrototypeSpec} */
  monopodial(lateralBranches, lateralSpread) {},
  /** An asymmetric fork, dominant primary arm (terminal 0) plus a secondary: decurrent spreading crowns (oak, maple). @param {number} [primarySpread=0.3] @param {number} [lateralSpread=0.7] @returns {FloraPrototypeSpec} */
  sympodial(primarySpread, lateralSpread) {},
  /** A shelf of `arms` (clamped 2-8) near-horizontal arms (pine, cedar, dogwood). @param {number} [arms=3] @param {number} [spread=0.85] @returns {FloraPrototypeSpec} */
  horizontalTier(arms, spread) {},
  /** Alias of horizontalTier. @param {number} [arms=3] @param {number} [spread=0.85] @returns {FloraPrototypeSpec} */
  tier(arms, spread) {},
  /** Pendulous shoots that arch out and droop (weeping willow, birch). @param {number} [spread=0.6] @param {number} [droop=0.4] @returns {FloraPrototypeSpec} */
  weeping(spread, droop) {},
};

/**
 * Build a leaf cluster / twig spray Mesh in local space: twig root at the
 * origin running along +Z to `twigLength`, +Y the light-facing side, +X the
 * spread axis. Vertex color R holds the wind-bend weight (0 at the twig
 * base, 1 at leaf tips). Also callable as `leafCluster(opts)` with
 * `opts.phyllotaxy`.
 * @example
 *  const spray = bro.flora.leafCluster('spiral', { count: 8, leafShape: 'pointed' });
 *  const leaves = world.emitFoliageMesh(spray, { perUnitLength: 6 });
 * @param {number|string} [phyllotaxy='alternate'] - A Phyllotaxy value or its
 *   name (either case; `'pinnate'` / `'compound_pinnate'` also work). Unknown
 *   values mean Alternate.
 * @param {Object} [opts]
 * @param {number} [opts.count=6] - Leaves / leaflets, an integer in [0, 4096]
 *   (RangeError otherwise).
 * @param {number} [opts.twigLength=0.25] @param {number} [opts.twigRadius=0.005]
 * @param {number} [opts.petioleLength=0.04]
 * @param {number} [opts.leafWidth=0.12] @param {number} [opts.leafLength=0.2]
 * @param {string|number} [opts.leafShape='oval'] - 'oval' | 'pointed' | 'lobed' |
 *   'needle' | 'frond' | 'petal' or 0-5; `shape` is an alias.
 * @param {number} [opts.leafBend=0.3] - Lengthwise bend, radians.
 * @param {number} [opts.leafCurl=0.1] - Axial twist, radians.
 * @param {number} [opts.leafCup=0.2] @param {number} [opts.droop=0.2]
 * @param {number} [opts.upBias=0.6] @param {number} [opts.spread=0.7] - Fan angle, radians.
 * @param {boolean} [opts.includeTwigMesh=true] - Emit the twig cylinder.
 * @param {boolean} [opts.shapedSilhouette=true] - Shape card width by leafShape.
 * @param {boolean} [opts.fullUV=false] - UVs span 0..1 instead of a 4x4 atlas cell.
 * @returns {Mesh}
 */
bro.flora.leafCluster = function(phyllotaxy, opts) {};

// ── Global wind, density and placement batches ───────────────────────────────
//
// Process-wide state shared by every world (see "Wind and density" at the
// top) plus a small JS-side registry of instanced placement batches that
// update() sways.

/**
 * Set the global wind. Throws without `strength`. The direction is a 2D
 * vector on the ground plane, `dirX` along world X and `dirY` along world Z;
 * a zero vector means +X.
 * @param {number} strength
 * @param {number} [dirX=0]
 * @param {number} [dirY=0]
 */
bro.flora.setWind = function(strength, dirX, dirY) {};

/**
 * With no arguments, returns `{strength, dirX, dirY}`; with arguments, the
 * same as setWind (returns undefined).
 * @param {number} [strength]
 * @param {number} [dirX=0]
 * @param {number} [dirY=0]
 * @returns {{strength: number, dirX: number, dirY: number}|undefined}
 */
bro.flora.wind = function(strength, dirX, dirY) {};

/**
 * Set the global foliage density multiplier (default 1). Throws without an
 * argument.
 * @param {number} density
 */
bro.flora.setDensity = function(density) {};

/**
 * With no argument, returns the density; with one, sets it.
 * @param {number} [density]
 * @returns {number|undefined}
 */
bro.flora.density = function(density) {};

/**
 * Advance the wind clock by `dt` seconds (throws without it) and re-sway
 * every placement batch's `transforms` from its `baseTransforms`.
 * @param {number} dt
 */
bro.flora.update = function(dt) {};

/** Reset wind, wind clock and density to defaults and drop every placement batch. */
bro.flora.clear = function() {};

/**
 * An instanced placement batch, as addPlacement returns it. `transforms`
 * (swayed by update) and `baseTransforms` (rest pose) are Float32Arrays of
 * 16 floats per instance with translation at [3], [7], [11].
 * @typedef {Object} FloraBatch
 * @property {string|*} id @property {*} mesh @property {*} material
 * @property {Float32Array|null} transforms @property {Float32Array|null} baseTransforms
 * @property {number} count @property {number} instanceCount
 * @property {number} windFactor @property {boolean} castShadow @property {boolean} receiveShadow
 * @property {*} aabb
 */

/**
 * Register a placement batch (or an array of them, returning an array).
 * Instances come from `config.transforms` (alias `instances`): a flat
 * Float32Array / number array of 16-float matrices, an array of 16-element
 * arrays, or an array of [x, y, z] positions (identity rotation). Failing
 * those, `config.count` identity instances: an integer in [0, 16777216],
 * TypeError for a non-number and RangeError for anything else.
 * @param {Object} config
 * @param {*} [config.id] - Default `'flora_batch_<n>'`.
 * @param {*} [config.mesh] @param {*} [config.material] - Stored as given.
 * @param {Float32Array|Array} [config.transforms]
 * @param {number} [config.count]
 * @param {number} [config.windFactor=1] - Scales the global wind for this batch.
 * @param {boolean} [config.castShadow=true] @param {boolean} [config.receiveShadow=true]
 * @param {*} [config.aabb]
 * @returns {FloraBatch|Array<FloraBatch>|null} null for a missing config.
 */
bro.flora.addPlacement = function(config) {};

/**
 * No argument: the registered placement configs. An array: replace every
 * batch with these and return the batches. An object: addPlacement.
 * @param {Object|Array<Object>} [config]
 * @returns {Array<Object>|Array<FloraBatch>|FloraBatch|null}
 */
bro.flora.placement = function(config) {};

/** @returns {Array<FloraBatch>} The live batch list. */
bro.flora.batches = function() {};

/** Alias of batches(). @returns {Array<FloraBatch>} */
bro.flora.getBatches = function() {};
