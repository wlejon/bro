// ── bro.mesh: sweeps, plants and L-systems ───────────────────────────────────
//
// The procedural half of bromesh: profile sweeps, leaf and flower cards,
// space-colonization branch trees, collision-aware leaf scattering and anchor
// packing, the shared `CapsuleField` obstacle substrate, and the `LSystem`
// rewriter with its turtle interpreter.
//
// Geometry basics are docs/mesh-api.js; file I/O, isosurfaces and `PolyMesh`
// are docs/mesh-io-api.js; `bro.rigging` is docs/rigging-api.js.
//
// Everything here is a static on `Mesh` and is also forwarded onto `bro.mesh`,
// except the `CapsuleField` and `LSystem` classes, which are globals and also
// hang off `bro.mesh`.
//
// Argument shapes, shared by every entry point below:
//   Vec3        [x,y,z] or {x,y,z}
//   Vec3 list   Float32Array(3N) | Float64Array(3N) | [[x,y,z]...] | [{x,y,z}...] | flat [x,y,z,x,y,z,...]
//   Vec2 list   Float32Array(2N) | [[x,y]...] | flat [x,y,x,y,...]
//   float-like  a single number, or one entry per ring/sample
//
// The usual pipeline:
//   const segs = Mesh.spaceColonize(attractors, [[0,0,0]], [0,1,0], opts);
//   Mesh.thickenBranches(segs, 0.02, 2.5);
//   const trunk = Mesh.meshBranches(segs, 8);
//   const leaf  = Mesh.leafCard('oval', { length: 0.3 });
//   const canopy = Mesh.scatterLeaves(segs, leaf, { perUnitLength: 30 });

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 *  One segment of a branch skeleton, as `spaceColonize` / `lsystemToBranches` /
 *  `tree` produce it and as `thickenBranches` / `meshBranches` /
 *  `placeLeavesOnBranches` / `scatterLeaves` consume it. Roots have
 *  `parent === -1` and `from === to === seedPoint`.
 * @typedef {Object} MeshBranchSegment
 * @property {number} [parent] -  index of the parent segment, -1 at a root.
 * @property {Array<number>} [from]
 * @property {Array<number>} [to]
 * @property {number} [radius] -  radius at `from`; 0 until `thickenBranches` assigns the pipe model.
 * @property {number} [depth]
 */

/**
 *  Capsule obstacle for `CapsuleField`: the segment a→b swept by `radius`.
 * @typedef {Object} MeshCapsule
 * @property {Array<number>} [a]
 * @property {Array<number>} [b]
 * @property {number} [radius]
 * @property {number} [tag] -  identity used by the `excludeTag` query parameters; -1 when absent.
 */

/**
 *  Sphere obstacle / keep-out volume.
 * @typedef {Object} MeshSphere
 * @property {Array<number>} [center]
 * @property {number} [radius]
 * @property {number} [tag]
 */

/**
 * @typedef {Object} MeshCapsuleFieldNearest
 * @property {Array<number>} [point]
 * @property {Array<number>} [normal]
 * @property {number} [distance]
 * @property {number} [tag]
 */

/**
 *  One L-system module. `symbol` is a single character.
 * @typedef {Object} MeshLSystemModule
 * @property {string} [symbol]
 * @property {Array<number>} [params]
 */

/**
 * @typedef {Object} MeshSweepOptions
 * @property {boolean} [closeProfile] -  connect the last profile vertex to the first. Default true.
 * @property {boolean} [capStart] -  default true.
 * @property {boolean} [capEnd] -  default true.
 * @property {boolean} [miterJoints] -  place interior rings on the tangent bisector so corners do not gap. Default true.
 * @property {number|Array<number>} [profileScale] -  one number, or one entry per path point.
 * @property {number|Array<number>} [twist] -  radians; one number, or one entry per path point.
 */

/**
 * @typedef {Object} MeshTubeOptions
 * @property {boolean} [capStart] -  default true.
 * @property {boolean} [capEnd] -  default true.
 * @property {boolean} [miterJoints] -  default true.
 */

/**
 * @typedef {Object} MeshBezierSweepOptions
 * @property {number} [samples] -  path samples along the spline, >= 4. Default 32.
 * @property {boolean} [capStart] -  default true.
 * @property {boolean} [capEnd] -  default true.
 * @property {boolean} [closeProfile] -  default true.
 * @property {boolean} [miterJoints] -  default true.
 * @property {number|Array<number>} [profileScale] -  one number, or one entry per sample.
 * @property {number|Array<number>} [twist]
 */

/**
 * @typedef {Object} MeshBlobOptions
 * @property {number} [radius] -  default 0.5.
 * @property {number} [seed] -  default 42.
 * @property {number} [nsub] -  subdivisions 0..3, default 2.
 * @property {number|Array<number>} [scale] -  uniform, or per-axis.
 * @property {Array<number>} [center] -  translation baked into the positions.
 */

/**
 *  `Mesh.leafCard` tuning. The card lies in the XZ plane with +Y as its
 *  normal, +Z toward the tip; vertex colours carry windBend in R (0 at the
 *  base, 1 at the tip).
 * @typedef {Object} MeshLeafCardOptions
 * @property {number} [width] -  full width along local X. Default 0.4.
 * @property {number} [length] -  full length along local Z. Default 1.
 * @property {number} [bend] -  total forward deflection at the tip, radians. Default 0.
 * @property {number} [curl] -  twist around the length axis, base to tip, radians. Default 0.
 * @property {number} [cup] -  bilateral cupping; 0.5-1.2 gives a true U cross-section. Default 0.
 * @property {boolean} [stemOffset] -  pivot at the base rather than the centre. Default true.
 * @property {number} [widthSegments] -  default 4.
 * @property {number} [lengthSegments] -  default 8; bend looks smooth from 8 up.
 * @property {boolean} [fullUV] -  span the whole [0,1] UV range instead of one atlas cell; `shape` then only affects the silhouette. Default false.
 * @property {boolean} [shapedSilhouette] -  vary the geometric width per `shape` instead of a flat rectangle. Default false.
 */

/**
 * @typedef {Object} MeshFlowerOptions
 * @property {number} [petalCount] -  default 6.
 * @property {string|number} [petalShape] -  a leaf-shape name; default 'petal'.
 * @property {number} [petalLength] -  default 0.5.
 * @property {number} [petalWidth] -  default 0.25.
 * @property {number} [petalCurl] -  radians of per-petal twist. Default 0.
 * @property {number} [petalBend] -  tip deflection, radians. Default 0.6.
 * @property {number} [layers] -  stacked rings; >1 is rose-like. Default 1.
 * @property {number} [layerTwist] -  radians between successive layers. Default 0.4.
 * @property {number} [centerRadius] -  default 0.08.
 * @property {number} [centerHeight] -  default 0.04.
 * @property {number} [outerTilt] -  pre-tilt of the outermost ring, radians; negative lifts the tip. Default -0.40.
 * @property {number} [innerTilt] -  the innermost ring's tilt. Default -0.25.
 * @property {number} [layerScaleFalloff] -  inner layers scale by 1 - falloff * t. Default 0.40.
 * @property {number} [outerYLift] -  per-layer Y lift in centerHeight units, layer 0. Default 0.40.
 * @property {number} [innerYLift] -  the innermost layer's lift. Default 0.80.
 * @property {number} [petalCup] -  bilateral cup per petal; 0.6-1.0 reads as a rose. Default 0.
 * @property {boolean} [shapedPetals] -  shaped silhouettes rather than rectangles. Default false.
 * @property {Array<number>} [centerColor] -  rgb, default [1, 0.85, 0.2].
 */

/**
 * @typedef {Object} MeshBladeStripOptions
 * @property {number} [width] -  half-extent of the diamond cross-section along profile X. Default 0.05.
 * @property {number} [thickness] -  half-extent along profile Y; 0 collapses to a flat strip. Default 0.
 * @property {number|Array<number>} [profileScale] -  one number, or one entry per path point.
 * @property {number|Array<number>} [twist] -  radians around the tangent.
 * @property {boolean} [capStart] -  default false.
 * @property {boolean} [capEnd] -  default true.
 * @property {boolean} [miterJoints] -  default true.
 */

/**
 * @typedef {Object} MeshBladePathOptions
 * @property {Array<number>} [base] -  default [0,0,0].
 * @property {Array<number>} [tipDir] -  normalized internally. Default [0,1,0].
 * @property {number} [length] -  default 1.
 * @property {number} [bend] -  lateral tip offset perpendicular to `tipDir`. Default 0.
 * @property {number} [lift] -  bow along world +Y, independent of `bend`. Default 0.
 * @property {number} [segments] -  default 8; the path has segments + 1 points.
 */

/**
 * @typedef {Object} MeshSpaceColonizationOptions
 * @property {number} [attractionRadius] -  default 5.
 * @property {number} [killRadius] -  attractors nearer than this to a node are consumed. Default 0.5.
 * @property {number} [segmentLength] -  step per growth iteration. Default 0.3.
 * @property {number} [maxIterations] -  default 200.
 * @property {Array<number>} [tropism] -  direction biased into every step. Default [0,0,0].
 * @property {number} [tropismWeight] -  default 0.
 * @property {CapsuleField} [obstacles] -  growth avoids it; null disables.
 * @property {number} [obstacleClearance] -  default 0.
 * @property {number} [obstacleSteer] -  radians a blocked step may rotate toward the obstacle tangent before being dropped; 0 = hard reject. Default 0.
 */

/**
 * @typedef {Object} MeshTreeOptions
 * @property {Array<number>} [base] -  trunk root. Default [0,0,0].
 * @property {Array<number>} [canopyCenter] -  centre of the attractor cloud. Default [0,4,0].
 * @property {number} [canopyRadius] -  default 3.
 * @property {number} [attractorCount] -  sampled uniformly in the sphere. Default 200.
 * @property {number} [sides] -  branch-tube ring resolution. Default 8.
 * @property {number} [leafRadius] -  tip radius for `thickenBranches`. Default 0.05.
 * @property {number} [pipeExp] -  pipe-model exponent. Default 2.5.
 * @property {number} [seed] -  default 1.
 * @property {MeshSpaceColonizationOptions} [colonize] -  space-colonization tuning for the skeleton pass.
 */

/**
 *  What `Mesh.tree()` returns: the thickened skeleton plus its swept branch
 *  mesh. Foliage is yours — feed `segments` to `scatterLeaves`.
 * @typedef {Object} MeshTreeResult
 * @property {Array<MeshBranchSegment>} [segments]
 * @property {Mesh} [branches]
 */

/**
 * @typedef {Object} MeshLeafPlacementOptions
 * @property {number} [maxRadius] -  skip branches thicker than this, to keep leaves off the trunk. Default 0.05.
 * @property {number} [minDepth] -  skip segments shallower than this. Default 1.
 * @property {boolean} [terminalOnly] -  only chain tips. Default false.
 * @property {number} [perUnitLength] -  average leaves per unit of segment length. Default 20.
 * @property {number} [densityFalloff] -  >0 biases samples toward the tip. Default 0.
 * @property {Array<number>} [densityWeight] -  per-segment multiplier on `perUnitLength`, indexed in lockstep with `segments`; the hook a simulation drives foliage with (broflora's per-segment `FoliageSample.mass`, say). Empty = uniform.
 * @property {number} [upBias] -  0 = leaf forward is purely radial, 1 = forced toward world up. Default 0.5.
 * @property {number} [tiltJitter] -  radians of random pitch. Default 0.3.
 * @property {number} [rollJitter] -  radians of random roll. Default 0.2.
 * @property {number} [baseScale] -  default 1.
 * @property {number} [scaleJitter] -  fraction of `baseScale`. Default 0.2.
 * @property {number} [scaleByRadius] -  0 ignores branch radius, 1 scales with radius/maxRadius. Default 0.
 * @property {number} [dedupRadius] -  minimum spacing between accepted origins; 0 disables. Default 0.
 * @property {CapsuleField} [avoid] -  reject leaves whose origin is within `obstacleClearance` of a non-self obstacle. Self-exclusion uses the segment index as the tag, so build it with `Mesh.capsuleFieldFromSegments`.
 * @property {number} [obstacleClearance] -  extra clearance on every avoid test. Default 0.
 * @property {number} [obstaclePushout] -  push a rejected candidate this far along the nearest normal and retry once; 0 = hard reject. Default 0.
 * @property {Array<MeshSphere>} [keepOut] -  extra keep-out spheres, e.g. volume reserved for blooms.
 * @property {number} [seed] -  default 0.
 */

/**
 *  Flat per-leaf instance buffer. `transforms` holds 16 floats per leaf in
 *  bro's InstancedMeshNode layout: floats 0-11 are a row-major 3x4 affine
 *  of T * R * uniform-S (rows 0-2 / 4-6 / 8-10 are the basis, translation
 *  at 3 / 7 / 11) and floats 12-15 an RGBA tint, written white. It is not
 *  a column-major 4x4. The leaf's local frame matches `leafCard` output
 *  (+Z tip, +Y card normal, +X side).
 * @typedef {Object} MeshPlacedLeaves
 * @property {number} [count]
 * @property {Float32Array} [transforms]
 * @property {Float32Array} [branchRadius] -  one per leaf, for shading variation.
 * @property {Int32Array} [branchDepth]
 */

/**
 * @typedef {Object} MeshAnchorPackOptions
 * @property {number} [minSpacing] -  minimum distance between accepted anchors; 0 disables. Default 0.
 * @property {number} [minObstacleDistance] -  ignored without `avoid`. Default 0.
 * @property {number} [maxCount] -  0 = unlimited. Default 0.
 * @property {number} [seed] -  seeds the candidate visit order. Default 0.
 * @property {CapsuleField} [avoid]
 * @property {Array<MeshSphere>} [keepOut]
 */

/**
 * @typedef {Object} MeshTurtleOptions
 * @property {number} [stepLength] -  forward step when F/G/f carries no length. Default 0.1.
 * @property {number} [angle] -  rotation in RADIANS when a rotation symbol carries no parameter. Default 0.4363 (25 degrees).
 * @property {number} [radius] -  default segment radius. Default 0.01.
 * @property {Array<number>} [position] -  default [0,0,0].
 * @property {Array<number>} [heading] -  initial forward. Default [0,1,0].
 * @property {Array<number>} [up] -  initial roll reference. Default [0,0,1].
 */

// ── Sweeps ───────────────────────────────────────────────────────────────────

/**
 * Extrude a closed 2D `profile` (in its local XY plane) along a 3D `path`
 * using parallel-transport frames, so the cross-section does not spin. End
 * caps triangulate the profile assuming it is convex.
 *
 * @param {Array<Array<number>>} profile
 * @param {Array<Array<number>>} path
 * @param {MeshSweepOptions} [opts]
 * @returns {Mesh}
 *
 * @example
 *   const square = [[-0.1,-0.1], [0.1,-0.1], [0.1,0.1], [-0.1,0.1]];
 *   const rail = Mesh.sweep(square, [[0,0,0], [0,1,0], [1,2,0]], { twist: 0.6 });
 */
Mesh.sweep = function(profile, path, opts) {};

/**
 *  Sweep a 2D profile along a cubic-Bézier polyline. `controlPoints` is a
 *  sequence of cubic segments sharing endpoints, so N must satisfy
 *  `(N - 1) % 3 === 0` and `N >= 4` — 4 points for one segment, 7 for two.
 *  Malformed input gives an empty Mesh.
 * @param {Array<Array<number>>} controlPoints
 * @param {Array<Array<number>>} profile
 * @param {MeshBezierSweepOptions} [opts]
 * @returns {Mesh}
 */
Mesh.bezierSweep = function(controlPoints, profile, opts) {};

/**
 *  Circular-cross-section sweep: the branch/vine/stem case. `path` needs at
 *  least 2 points. `radius` is one number or one entry per path point;
 *  `sides` is the ring resolution (>= 3).
 * @param {Array<Array<number>>} path
 * @param {number|Array<number>} [radius=0.1]
 * @param {number} [sides=8]
 * @param {MeshTubeOptions} [opts]
 * @returns {Mesh}
 */
Mesh.tube = function(path, radius, sides, opts) {};

// ── Plant cards ──────────────────────────────────────────────────────────────
//
// Leaf and flower shapes are named: 'oval', 'pointed', 'lobed', 'needle',
// 'frond', 'petal' (the integers 0..5 also work, in that order). The name
// picks a cell in a 4x4 UV atlas — oval (0,0), pointed (1,0), lobed (2,0),
// needle (3,0), frond (0,1), petal (1,1) — and, with `shapedSilhouette`, the
// geometric outline too. Supplying that atlas is the caller's job.

/**
 *  Noise-displaced sphere with non-uniform scale and a translation baked in —
 *  a `.scale().translate()` round-trip saved, for stamping canopy blobs. A
 *  leading number reads positionally as `(radius, seed, nsub)`.
 * @param {MeshBlobOptions|number} [opts]
 * @returns {Mesh}
 */
Mesh.blob = function(opts) {};

/**
 * A bent / curled / cupped leaf or petal card.
 *
 * @param {string|number} shape
 * @param {MeshLeafCardOptions} [opts]
 * @returns {Mesh}
 *
 * @example
 *   const petal = Mesh.leafCard('petal', {
 *     length: 0.4, width: 0.3, bend: 0.5, cup: 0.9, shapedSilhouette: true,
 *   });
 */
Mesh.leafCard = function(shape, opts) {};

/**
 *  A radial flower: a small dome centre with `petalCount` leaf cards in
 *  `layers` rings, each ring shrinking and rotating by `layerTwist`. One
 *  merged Mesh with positions, normals, UVs and colours.
 * @param {MeshFlowerOptions} [opts]
 * @returns {Mesh}
 */
Mesh.flower = function(opts) {};

/**
 *  Sweep a 4-vertex diamond profile along `path`: grass, fern leaflets,
 *  succulent blades.
 * @param {Array<Array<number>>} path
 * @param {MeshBladeStripOptions} [opts]
 * @returns {Mesh}
 */
Mesh.bladeStrip = function(path, opts) {};

/**
 *  A quadratic-Bézier blade spine as `[[x,y,z], ...]`, ready for `bladeStrip`
 *  or `sweep`.
 * @param {MeshBladePathOptions} [opts]
 * @returns {Array<Array<number>>}
 *
 * @example
 *   const blade = Mesh.bladeStrip(
 *     Mesh.bladePath({ length: 0.8, bend: 0.25, lift: 0.1 }),
 *     { width: 0.02 });
 */
Mesh.bladePath = function(opts) {};

// ── Branch trees ─────────────────────────────────────────────────────────────

/**
 * Runions-style space colonization. Returns segments in creation order, each
 * segment's parent indexing an earlier entry; one root per seed point, with
 * `parent === -1`.
 *
 * @param {Array<Array<number>>} attractors
 * @param {Array<Array<number>>} seedPoints
 * @param {Array<number>} initialDirection
 * @param {MeshSpaceColonizationOptions} [opts]
 * @returns {Array<MeshBranchSegment>}
 */
Mesh.spaceColonize = function(attractors, seedPoints, initialDirection, opts) {};

/**
 *  Assign radii by the pipe model: tips get `leafRadius`, and each parent is
 *  `(sum of child^pipeExp)^(1/pipeExp)`. Returns a new segment array with the
 *  radii filled in.
 * @param {Array<MeshBranchSegment>} segments
 * @param {number} [leafRadius=0.02]
 * @param {number} [pipeExp=2.5]
 * @returns {Array<MeshBranchSegment>}
 */
Mesh.thickenBranches = function(segments, leafRadius, pipeExp) {};

/**
 *  Sweep every segment as a tube and merge them into one mesh.
 * @param {Array<MeshBranchSegment>} segments
 * @param {number} [sides=8] -  clamped to at least 3.
 * @returns {Mesh}
 */
Mesh.meshBranches = function(segments, sides) {};

/**
 *  `spaceColonize` → `thickenBranches` → `meshBranches` in one call.
 * @param {MeshTreeOptions} [opts]
 * @returns {MeshTreeResult}
 *
 * @example
 *   const { segments, branches } = Mesh.tree({ canopyRadius: 2.5, seed: 7 });
 *   const foliage = Mesh.scatterLeaves(segments, Mesh.leafCard('oval'), {
 *     avoid: Mesh.capsuleFieldFromSegments(segments),
 *   });
 */
Mesh.tree = function(opts) {};

// ── Leaf scattering and anchor packing ───────────────────────────────────────

/**
 *  Compute leaf instance transforms along branch segments, without building
 *  geometry — feed `transforms` to an instanced draw.
 * @param {Array<MeshBranchSegment>} segments
 * @param {MeshLeafPlacementOptions} [opts]
 * @returns {MeshPlacedLeaves}
 */
Mesh.placeLeavesOnBranches = function(segments, opts) {};

/**
 *  The same placement, but stamping `leaf` at every transform and returning
 *  one merged mesh.
 * @param {Array<MeshBranchSegment>} segments
 * @param {Mesh} leaf
 * @param {MeshLeafPlacementOptions} [opts]
 * @returns {Mesh}
 */
Mesh.scatterLeaves = function(segments, leaf, opts) {};

/**
 * Greedy spaced-anchor picker for blooms, fruit and clustered foliage. Visits
 * candidates in a seeded shuffled order and accepts each one that clears every
 * accepted anchor by `minSpacing`, clears the obstacle field by
 * `minObstacleDistance`, and lies outside every `keepOut` sphere. Returns the
 * accepted candidate INDICES, in acceptance order.
 *
 * @param {Array<Array<number>>} candidates
 * @param {MeshAnchorPackOptions} [opts]
 * @returns {Int32Array}
 *
 * @example
 *   const tips = segments.filter(s => s.radius < 0.01).map(s => s.to);
 *   const picked = Mesh.packAnchors(tips, { minSpacing: 0.2, maxCount: 24 });
 */
Mesh.packAnchors = function(candidates, opts) {};

/**
 *  Build a `CapsuleField` directly. Same as `new CapsuleField(...)`.
 * @param {Array<MeshCapsule>} [capsules]
 * @param {Array<MeshSphere>} [spheres]
 * @param {number} [cellSize=0] -  0 picks a cell size from the contents.
 * @returns {CapsuleField}
 */
Mesh.capsuleField = function(capsules, spheres, cellSize) {};

/**
 *  A `CapsuleField` whose capsules carry their segment index as `tag`, so leaf
 *  placement can exclude a leaf's own branch. This is the field
 *  `MeshLeafPlacementOptions.avoid` expects.
 * @param {Array<MeshBranchSegment>} segments
 * @param {number} [radiusScale=1]
 * @param {Array<MeshSphere>} [extraSpheres]
 * @returns {CapsuleField}
 */
Mesh.capsuleFieldFromSegments = function(segments, radiusScale, extraSpheres) {};

// ── CapsuleField ─────────────────────────────────────────────────────────────

/**
 * Capsule plus sphere occupancy field over a uniform grid: the shared obstacle
 * substrate for `spaceColonize`, `placeLeavesOnBranches`, `scatterLeaves` and
 * `packAnchors`, and useful on its own for "is this spot free" tests.
 *
 * Queries take a point as [x,y,z] or {x,y,z}. `excludeTag` skips obstacles
 * carrying that tag — how a leaf ignores the branch it grows from.
 *
 * @example
 *   const field = Mesh.capsuleFieldFromSegments(segments, 1.1);
 *   if (!field.tooClose(candidate, 0.05)) place(candidate);
 */
class CapsuleField {

  /**
   * @param {Array<MeshCapsule>} [capsules]
   * @param {Array<MeshSphere>} [spheres]
   * @param {number} [cellSize=0]
   */
  constructor(capsules, spheres, cellSize) {}

  /** @readonly @type {boolean} */
  empty;

  /** @readonly @type {number} */
  capsuleCount;

  /** @readonly @type {number} */
  sphereCount;

  /** @readonly @type {number} */
  cellSize;

  /**
   *  Is the point inside an obstacle, inflated by `extraClearance`?
   * @param {Array<number>} point @param {number} [excludeTag=-1] @param {number} [extraClearance=0]
   * @returns {boolean}
   */
  contains(point, excludeTag, extraClearance) {}

  /**
   * @param {Array<number>} point @param {number} [clearance=0] @param {number} [excludeTag=-1]
   * @returns {boolean}
   */
  tooClose(point, clearance, excludeTag) {}

  /**
   *  Signed distance to the nearest obstacle surface; negative inside.
   * @param {Array<number>} point @param {number} [excludeTag=-1]
   * @returns {number}
   */
  distance(point, excludeTag) {}

  /**
   * @param {Array<number>} point @param {number} [excludeTag=-1]
   * @returns {MeshCapsuleFieldNearest|null}
   */
  nearest(point, excludeTag) {}

  /**
   * @param {Array<number>} center @param {number} radius @param {number} [excludeTag=-1]
   * @returns {boolean}
   */
  intersectsSphere(center, radius, excludeTag) {}

}

// ── L-systems ────────────────────────────────────────────────────────────────
//
// A module is `{ symbol: string, params: number[] }`. The compact text form is
// `F(1.0)[+(25)F]F`: a symbol is any non-whitespace character other than `(`,
// `)` or `,`, optionally followed by a parenthesized comma-separated float
// list. Whitespace is ignored, and `[` / `]` pass through unchanged when no
// rule matches them.

/**
 *  Parse a compact L-system string into a module list. A parse error gives [].
 * @param {string} text
 * @returns {Array<MeshLSystemModule>}
 */
Mesh.parseLSystem = function(text) {};

/**
 * Interpret a flat module sequence as a 3D turtle and return the branch
 * segments it traces, in the same shape `Mesh.spaceColonize` produces — so the
 * result drops straight into `thickenBranches` / `meshBranches` /
 * `scatterLeaves`. A compact string is accepted in place of the module array.
 *
 * Recognised symbols (numeric params override the defaults):
 *   F(len?, r?)      forward, emitting a segment; the second param sets THIS
 *                    segment's radius without changing the active radius
 *   G(len?), f(len?) forward without emitting
 *   +(deg?), -(deg?) yaw around `up`
 *   &(deg?), ^(deg?) pitch around the left axis (heading x up)
 *   \(deg?), /(deg?) roll around `heading`
 *   |                turn 180 degrees around `up`
 *   !(r)             set the active radius for subsequent F segments
 *   [ , ]            push / pop turtle state
 *
 * Params on rotation symbols are DEGREES; `opts.angle` is radians and applies
 * only when a rotation symbol has no parameter. Unknown symbols are ignored,
 * so a grammar can carry its own annotations.
 *
 * @param {Array<MeshLSystemModule>|string} modules
 * @param {MeshTurtleOptions} [opts]
 * @returns {Array<MeshBranchSegment>}
 *
 * @example
 *   const mods = Mesh.parseLSystem('!(0.04)F[+(35)F!(0.02)F][-(35)F!(0.02)F]F');
 *   const segs = Mesh.lsystemToBranches(mods, { stepLength: 0.5 });
 *   const trunk = Mesh.meshBranches(segs, 6);
 */
Mesh.lsystemToBranches = function(modules, opts) {};

/**
 * Stochastic string-rule L-system rewriter. Parametric rules with conditions
 * stay native-only; `deriveModules` hands out the module stream for callers
 * who want to interpret it themselves.
 *
 * @example
 *   const ls = new LSystem('F')
 *     .addRule('F', 'F[+F]F[-F]F', 2)
 *     .addRule('F', 'F[+F]F', 1);
 *   const segs = Mesh.lsystemToBranches(ls.deriveModules(4, 12345));
 */
class LSystem {

  /** @param {string} [axiom] -  compact-form starting word. */
  constructor(axiom) {}

  /** @param {string} text @returns {LSystem} this */
  setAxiom(text) {}

  /**
   *  Add a production. Rules sharing a predecessor are picked stochastically
   *  by `weight`.
   * @param {string} predecessor -  a single-character symbol; empty throws.
   * @param {string} successor -  compact-form replacement.
   * @param {number} [weight=1]
   * @returns {LSystem} this
   */
  addRule(predecessor, successor, weight) {}

  /**
   *  Run `iterations` rewrite passes. Deterministic given `seed`.
   * @param {number} iterations @param {number} [seed=0]
   * @returns {string} compact-form serialization of the derived modules
   */
  derive(iterations, seed) {}

  /**
   *  `derive` without the re-parse: the structured module list directly.
   * @param {number} iterations @param {number} [seed=0]
   * @returns {Array<MeshLSystemModule>}
   */
  deriveModules(iterations, seed) {}

}
