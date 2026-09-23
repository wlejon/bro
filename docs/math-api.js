// ── Classes & Interfaces ─────────────────────────────────────────────────────

/**
 * Uniform-grid 3D spatial hash over int32 ids. Ids are UNIQUE KEYS: each id
 * names at most one entry, so inserting an id that is already present MOVES
 * that entry to the new position (or sphere) instead of adding a second copy.
 * To move an object, insert it again; there is no need to remove it first.
 * Ids are converted to int32.
 *
 * Positions and query bounds of any size are defined: cell coordinates clamp
 * to +-2^30, so a huge, infinite or NaN position still has a cell, and a
 * query box that is very large or infinite falls back to scanning every entry
 * rather than walking cells. Every match is checked against the real distance.
 */
class SpatialHash3D {

  /**
   * Create a 3D spatial hash index.
   *
   * @param {number} [cellSize=1]  Grid cell edge length. Non-positive values
   *   are clamped to 1.
   */
  constructor(cellSize) {}

  /**
   * Total number of indexed entries (one per distinct id).
   * @readonly
   * @type {number}
   */
  size;

  /**
   * The cell edge length in use.
   * @readonly
   * @type {number}
   */
  cellSize;

  /**
   * Largest sphere radius ever inserted (queries dilate by it). It is never
   * tightened by remove(); clear() or reset() resets it.
   * @readonly
   * @type {number}
   */
  maxRadius;

  /**
   * Insert point id at [x, y, z]. If id is already in the index, its entry
   * moves here (a point or sphere entry becomes this point).
   *
   * Ids are int32: a fractional id truncates toward zero, and a NaN, an
   * infinity or a value outside -2^31 .. 2^31-1 throws a RangeError (here,
   * in insertSphere() and in remove()) rather than wrapping onto another id.
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} id
   * @returns {SpatialHash3D}
   */
  insert(x, y, z, id) {}

  /**
   * Insert a sphere of radius r centred at [x, y, z]. It matches a radius
   * query when its surface comes within the query radius. As with insert(),
   * an id already present moves.
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} r
   * @param {number} id
   * @returns {SpatialHash3D}
   */
  insertSphere(x, y, z, r, id) {}

  /**
   * Remove the entry with this id, in O(1). An unknown id is a no-op.
   *
   * @param {number} id
   * @returns {SpatialHash3D}
   */
  remove(id) {}

  /**
   * Find all ids within radius of [x, y, z] (alias: radiusQuery). A point
   * matches when its distance is <= radius, so radius 0 finds points exactly
   * at the centre; a negative or NaN radius matches nothing.
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} radius
   * @returns {Array<number>}
   */
  queryRadius(x, y, z, radius) {}

  /**
   * Find all ids whose point lies in, or whose sphere touches, the box.
   *
   * @param {number} minX
   * @param {number} minY
   * @param {number} minZ
   * @param {number} maxX
   * @param {number} maxY
   * @param {number} maxZ
   * @returns {Array<number>}
   */
  queryAABB(minX, minY, minZ, maxX, maxY, maxZ) {}

  /**
   * Nearest id whose centre lies within maxDist of [x, y, z], by centre
   * distance (sphere radii are ignored). Returns -1 when none is in range or
   * maxDist is not > 0.
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} [maxDist=Infinity]
   * @returns {number}
   */
  nearest(x, y, z, maxDist) {}

  /**
   * Remove every entry.
   * @returns {SpatialHash3D}
   */
  clear() {}

  /**
   * Change the cell size and remove every entry.
   *
   * @param {number} [cellSize=1]  Non-positive values are clamped to 1.
   * @returns {SpatialHash3D}
   */
  reset(cellSize) {}

}

class Rng {

  /**
   * Deterministic SplitMix64 pseudo-random number generator.
   *
   * @param {number} [seed=0]
   */
  constructor(seed) {}

  /**
   * Reset seed state.
   *
   * @param {number} seed
   * @returns {Rng}
   */
  reseed(seed) {}

  /**
   * Uniform float in [0, 1).
   * @returns {number}
   */
  float01() {}

  /**
   * Uniform float in [-1, 1).
   * @returns {number}
   */
  signed() {}

  /**
   * Uniform float in [lo, hi).
   *
   * @param {number} lo
   * @param {number} hi
   * @returns {number}
   */
  range(lo, hi) {}

  /**
   * Uniform integer in [lo, hi] inclusive.
   *
   * @param {number} lo
   * @param {number} hi
   * @returns {number}
   */
  _int(lo, hi) {}

  /**
   * 32-bit unsigned random integer.
   * @returns {number}
   */
  uint32() {}

  /**
   * Standard normal Gaussian distribution (mean 0, stddev 1).
   * @returns {number}
   */
  normal() {}

  /**
   * 2D Gaussian point with standard deviation sigma.
   *
   * @param {number} sigma
   * @returns {Object}
   */
  gaussian2D(sigma) {}

  /**
   * Uniform random point inside unit disc {x, y}.
   * @returns {Object}
   */
  inUnitDisc() {}

  /**
   * Uniform random point inside unit sphere {x, y, z}.
   * @returns {Object}
   */
  inUnitSphere() {}

  /**
   * Uniform random point on unit sphere surface {x, y, z}.
   * @returns {Object}
   */
  onUnitSphere() {}

}

class Smoother {

  /**
   * Single-pole exponential signal filter / ramp.
   *
   * @param {number} [timeMs]
   * @param {number} [sampleRate]
   */
  constructor(timeMs, sampleRate) {}

  /**
   * Current filter output value.
   * @readonly
   * @type {number}
   */
  current;

  /**
   * Target value being chased.
   * @readonly
   * @type {number}
   */
  target;

  /**
   * Exponential decay coefficient.
   * @readonly
   * @type {number}
   */
  coeff;

  /**
   * Set 95% closure time and sample rate.
   *
   * @param {number} timeMs
   * @param {number} sampleRate
   * @returns {Smoother}
   */
  setTime(timeMs, sampleRate) {}

  /**
   * Snap filter state to value without ramping.
   *
   * @param {number} value
   * @returns {Smoother}
   */
  reset(value) {}

  /**
   * Set target value to chase.
   *
   * @param {number} t
   * @returns {Smoother}
   */
  setTarget(t) {}

  /**
   * Advance one simulation tick.
   * @returns {number}
   */
  tick() {}

  /**
   * Advance n simulation ticks.
   *
   * @param {number} n
   * @returns {number}
   */
  tickN(n) {}

}

// ── Namespaces ───────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro.math — fast vector math, curves, RNG, smoothing, and spatial index
 * =============================================================================
 *
 * Comprehensive mathematics and geometry utilities for 2D/3D games and simulations.
 * Includes SpatialHash3D spatial indexing, SplitMix64 deterministic PRNG, single-pole
 * exponential signal smoothing, splines/curves, color conversion, and raycast intersection queries.
 *
 * bromath behaviour that shows up outside bro.math:
 *   - Quaternion to Euler (the rotationX/Y/Z read back from a SceneNode after
 *     a quaternion, lookAt, animation or physics rotation). The triple is
 *     XYZ radians, the inverse of q = qz * qy * qx. At the gimbal pole
 *     (|pitch| = pi/2) roll is folded into yaw: rotationX reads 0 and
 *     rotationZ carries the combined angle, which is now the correct angle
 *     (it used to read +-pi/2 whatever the rotation was). The fold starts
 *     within about 1e-3 rad of the pole, where the separate roll and yaw are
 *     rounding noise. So near straight up or down, rotationX and rotationZ do
 *     not read back as written, but the rotation they describe is the same.
 *   - Matrix inverse (camera view matrix, inverse world transforms for
 *     decals, probes, splats and normals). Singularity is judged relative to
 *     the matrix's own scale, not by a fixed determinant cutoff, so a small
 *     but valid transform (a node at scale 1e-5, say) inverts correctly
 *     instead of being treated as singular. A zero-scale or non-finite matrix
 *     is still singular and inverts to the identity.
 * @example
 * const hash = new bro.math.SpatialHash3D(2.0);
 *   hash.insert(10.0, 5.0, -2.0, 1);   // x, y, z, id
 *   hash.insert(11.0, 5.0, -2.0, 1);   // same id: the entry moves, size stays 1
 *   const nearby = hash.queryRadius(10.0, 5.0, -2.0, 5.0);
 * @example
 * const rng = new bro.math.Rng(12345);
 *   const p = rng.inUnitSphere();
 *   const v = bro.math.lerp(0.0, 100.0, 0.5);
 */
/**
 * Centripetal Catmull-Rom (alpha 0.5) point between p1 (t = 0) and p2
 * (t = 1), shaped by the neighbours p0 and p3. Points are {x, y, z} objects
 * or [x, y, z] arrays; a non-object point is a TypeError.
 *
 * A repeated end point (p0 equal to p1, or p3 equal to p2, the usual way to
 * start or end a spline) is replaced by the reflection of the other
 * neighbour, so the end segment stays curved instead of becoming a straight
 * lerp. A zero-length segment (p1 equal to p2) returns p1; if the knots still
 * coincide (non-finite input) the result is a plain lerp from p1 to p2.
 *
 * @param {{x:number,y:number,z:number}|number[]} p0
 * @param {{x:number,y:number,z:number}|number[]} p1
 * @param {{x:number,y:number,z:number}|number[]} p2
 * @param {{x:number,y:number,z:number}|number[]} p3
 * @param {number} t
 * @returns {{x:number,y:number,z:number}|null}  null with fewer than 5 arguments.
 */
bro.math.catmullRom = function(p0, p1, p2, p3, t) {};

/**
 * Linear interpolation between scalars a and b by factor t.
 *
 * @param {number} a
 * @param {number} b
 * @param {number} t
 * @returns {number}
 */
bro.math.lerp = function(a, b, t) {};

/**
 * Clamp scalar x into [lo, hi].
 *
 * @param {number} x
 * @param {number} lo
 * @param {number} hi
 * @returns {number}
 */
bro.math.clamp = function(x, lo, hi) {};

/**
 * Clamp scalar x into [0, 1].
 *
 * @param {number} x
 * @returns {number}
 */
bro.math.saturate = function(x) {};

/**
 * Inverse lerp computing position of x in [a, b].
 *
 * @param {number} a
 * @param {number} b
 * @param {number} x
 * @returns {number}
 */
bro.math.invLerp = function(a, b, x) {};

/**
 * Remap scalar x from input range to output range.
 *
 * @param {number} x
 * @param {number} inMin
 * @param {number} inMax
 * @param {number} outMin
 * @param {number} outMax
 * @returns {number}
 */
bro.math.remap = function(x, inMin, inMax, outMin, outMax) {};

/**
 * Smooth Hermite interpolation between edges e0 and e1.
 *
 * @param {number} e0
 * @param {number} e1
 * @param {number} x
 * @returns {number}
 */
bro.math.smoothstep = function(e0, e1, x) {};

/**
 * Higher-order C2-continuous smoothstep.
 *
 * @param {number} e0
 * @param {number} e1
 * @param {number} x
 * @returns {number}
 */
bro.math.smootherstep = function(e0, e1, x) {};

/**
 * Convert degrees to radians.
 *
 * @param {number} deg
 * @returns {number}
 */
bro.math.degToRad = function(deg) {};

/**
 * Convert radians to degrees.
 *
 * @param {number} rad
 * @returns {number}
 */
bro.math.radToDeg = function(rad) {};

/**
 * Wrap angle in radians to [-PI, PI].
 *
 * @param {number} a
 * @returns {number}
 */
bro.math.wrapAngle = function(a) {};

/**
 * Shortest signed angular difference from a to b in radians.
 *
 * @param {number} a
 * @param {number} b
 * @returns {number}
 */
bro.math.angleDiff = function(a, b) {};

/**
 * 32-bit FNV-1a string hash with optional seed.
 *
 * @param {string} data
 * @param {number} [seed]
 * @returns {number}
 */
bro.math.fnv1a32 = function(data, seed) {};

/**
 * Integer 32-bit hash.
 *
 * @param {number} x
 * @returns {number}
 */
bro.math.hashU32 = function(x) {};

/**
 * 2D/3D integer grid cell coordinate hash.
 *
 * @param {number} x
 * @param {number} y
 * @param {number} [z]
 * @returns {number}
 */
bro.math.cellHash = function(x, y, z) {};

