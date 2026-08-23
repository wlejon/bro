// ── Classes & Interfaces ─────────────────────────────────────────────────────

class SpatialHash3D {

  /**
   * Create a 3D spatial hash index.
   *
   * @param {number} [cellSize=1]
   * @param {number} [bucketCount=1024]
   */
  constructor(cellSize, bucketCount) {}

  /**
   * Total number of indexed entries.
   * @readonly
   * @type {number}
   */
  size;

  /**
   * Insert point ID into cell at [x, y, z].
   *
   * @param {number} id
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {SpatialHash3D}
   */
  insert(id, x, y, z) {}

  /**
   * Remove point ID from cell at [x, y, z].
   *
   * @param {number} id
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {boolean}
   */
  remove(id, x, y, z) {}

  /**
   * Find all point IDs within radius of [x, y, z].
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} radius
   * @returns {Array<number>}
   */
  queryRadius(x, y, z, radius) {}

  /**
   * Find all point IDs within bounding box.
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
   * Find nearest point ID within maxDist.
   *
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} maxDist
   * @returns {Object|null}
   */
  nearest(x, y, z, maxDist) {}

  /**
   * Clear all index buckets.
   * @returns {SpatialHash3D}
   */
  clear() {}

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
 * @example
 * const hash = new bro.math.SpatialHash3D(2.0, 4096);
 *   hash.insert(1, 10.0, 5.0, -2.0);
 *   const nearby = hash.queryRadius(10.0, 5.0, -2.0, 5.0);
 * @example
 * const rng = new bro.math.Rng(12345);
 *   const p = rng.inUnitSphere();
 *   const v = bro.math.lerp(0.0, 100.0, 0.5);
 */
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

