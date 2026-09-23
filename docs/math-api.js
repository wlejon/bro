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

/**
 * Hash a position's cell (of side cellSize) into a bucket index in
 * [0, bucketCount). A bucketCount of 0 gives 0.
 *
 * @param {Vec3Like} p
 * @param {number} cellSize
 * @param {number} bucketCount
 * @returns {number}
 */
bro.math.positionToCell = function(p, cellSize, bucketCount) {};

// ── Shared shapes for the geometry helpers below ────────────────────────────

/**
 * A 3D point or vector: an {x, y, z} object or an [x, y, z] array. A
 * non-object where a vector is expected is a TypeError ("expected vector").
 * @typedef {{x:number,y:number,z:number}|number[]} Vec3Like
 */

/**
 * A ray hit. `t` is in units of the ray direction (pass a unit direction for
 * a Euclidean distance); `point` = origin + direction * t.
 * @typedef {Object} MathRayHit
 * @property {number} t
 * @property {{x:number,y:number,z:number}} point
 * @property {{x:number,y:number,z:number}} normal
 */

/**
 * A 2D grid footprint for the grid* helpers: `origin` is the world XY of
 * cell (0, 0)'s corner ({x, y} or [x, y]), `width` columns by `depth` rows of
 * side `cellSize`.
 * @typedef {Object} MathGrid2D
 * @property {{x:number,y:number}|number[]} origin
 * @property {number} cellSize
 * @property {number} width
 * @property {number} depth
 */

// ── Curves ──────────────────────────────────────────────────────────────────

/**
 * CSS cubic-bezier easing: the curve through (0,0), (p1x,p1y), (p2x,p2y),
 * (1,1), sampled at x in [0, 1] (t is solved from x by Newton iteration).
 * Returns 0 with fewer than 5 arguments.
 *
 * @param {number} p1x
 * @param {number} p1y
 * @param {number} p2x
 * @param {number} p2y
 * @param {number} x
 * @returns {number}
 */
bro.math.cubicEase = function(p1x, p1y, p2x, p2y, x) {};

/**
 * Cubic Bezier point at t through control points p0..p3.
 *
 * @param {Vec3Like} p0
 * @param {Vec3Like} p1
 * @param {Vec3Like} p2
 * @param {Vec3Like} p3
 * @param {number} t
 * @returns {{x:number,y:number,z:number}|null}  null with fewer than 5 arguments.
 */
bro.math.bezier = function(p0, p1, p2, p3, t) {};

/**
 * Derivative (unnormalised tangent) of the cubic Bezier at t.
 *
 * @param {Vec3Like} p0
 * @param {Vec3Like} p1
 * @param {Vec3Like} p2
 * @param {Vec3Like} p3
 * @param {number} t
 * @returns {{x:number,y:number,z:number}|null}  null with fewer than 5 arguments.
 */
bro.math.bezierTangent = function(p0, p1, p2, p3, t) {};

/**
 * Cubic Hermite point at t from p0 (tangent m0) to p1 (tangent m1).
 *
 * @param {Vec3Like} p0
 * @param {Vec3Like} m0
 * @param {Vec3Like} p1
 * @param {Vec3Like} m1
 * @param {number} t
 * @returns {{x:number,y:number,z:number}|null}  null with fewer than 5 arguments.
 */
bro.math.hermite = function(p0, m0, p1, m1, t) {};

// ── Color ───────────────────────────────────────────────────────────────────
// Colors are {r, g, b, a} with linear-light channels in [0, 1].

/**
 * Parse "#RRGGBB" or "#RRGGBBAA" (sRGB) into a linear color. Malformed input
 * gives transparent black; no argument gives opaque black.
 *
 * @param {string} hex
 * @returns {{r:number,g:number,b:number,a:number}}
 */
bro.math.fromHex = function(hex) {};

/**
 * HSV (treated as sRGB) to a linear color: h in degrees [0, 360), s and v in
 * [0, 1], alpha default 1.
 *
 * @param {number} h
 * @param {number} s
 * @param {number} v
 * @param {number} [alpha]
 * @returns {{r:number,g:number,b:number,a:number}}
 */
bro.math.fromHSV = function(h, s, v, alpha) {};

/**
 * 8-bit sRGB channels (each saturated to [0, 255]; alpha default 255) to a
 * linear color. Alpha is linear in both.
 *
 * @param {number} r
 * @param {number} g
 * @param {number} b
 * @param {number} [a]
 * @returns {{r:number,g:number,b:number,a:number}}
 */
bro.math.fromColor8 = function(r, g, b, a) {};

/**
 * Linear channels (alpha default 1) to rounded 8-bit sRGB. Channels are
 * saturated to [0, 1] first; a NaN channel encodes as 0.
 *
 * @param {number} r
 * @param {number} g
 * @param {number} b
 * @param {number} [a]
 * @returns {{r:number,g:number,b:number,a:number}}
 */
bro.math.toColor8 = function(r, g, b, a) {};

/**
 * One channel, linear light to the sRGB transfer curve.
 * @param {number} c
 * @returns {number}
 */
bro.math.linearToSrgb = function(c) {};

/**
 * One channel, sRGB transfer curve to linear light.
 * @param {number} c
 * @returns {number}
 */
bro.math.srgbToLinear = function(c) {};

// ── Angles (aliases and extras) ─────────────────────────────────────────────

/** Alias of degToRad. @param {number} deg @returns {number} */
bro.math.deg2rad = function(deg) {};

/** Alias of radToDeg. @param {number} rad @returns {number} */
bro.math.rad2deg = function(rad) {};

/**
 * Fold an angle in radians into [0, 2*PI).
 * @param {number} a
 * @returns {number}
 */
bro.math.wrapAngle2Pi = function(a) {};

/**
 * Alias of angleDiff: shortest signed delta from `from` to `to`, in [-PI, PI].
 * @param {number} from
 * @param {number} to
 * @returns {number}
 */
bro.math.angleDelta = function(from, to) {};

/**
 * Interpolate between two angles (radians) the short way round.
 * @param {number} from
 * @param {number} to
 * @param {number} t
 * @returns {number}
 */
bro.math.angleLerp = function(from, to, t) {};

// ── Ray queries ─────────────────────────────────────────────────────────────
// Each returns a MathRayHit, or null on a miss or with too few arguments.

/**
 * Slab test. From outside, the entry point and the entered face's outward
 * normal; from inside, the exit point and the exited face's normal. A zero
 * direction hits nothing.
 *
 * @param {Vec3Like} origin
 * @param {Vec3Like} dir
 * @param {Vec3Like} boxMin
 * @param {Vec3Like} boxMax
 * @returns {MathRayHit|null}
 */
bro.math.rayIntersectAABB = function(origin, dir, boxMin, boxMax) {};

/**
 * Nearest non-negative hit on a sphere.
 * @param {Vec3Like} origin
 * @param {Vec3Like} dir
 * @param {Vec3Like} center
 * @param {number} radius
 * @returns {MathRayHit|null}
 */
bro.math.rayIntersectSphere = function(origin, dir, center, radius) {};

/**
 * Hit on the plane dot(normal, p) + d = 0.
 * @param {Vec3Like} origin
 * @param {Vec3Like} dir
 * @param {Vec3Like} normal
 * @param {number} d
 * @returns {MathRayHit|null}
 */
bro.math.rayIntersectPlane = function(origin, dir, normal, d) {};

/**
 * Moller-Trumbore triangle test; single-sided when backfaceCull is true.
 * @param {Vec3Like} origin
 * @param {Vec3Like} dir
 * @param {Vec3Like} v0
 * @param {Vec3Like} v1
 * @param {Vec3Like} v2
 * @param {boolean} [backfaceCull]
 * @returns {MathRayHit|null}
 */
bro.math.rayIntersectTriangle = function(origin, dir, v0, v1, v2, backfaceCull) {};

// ── Plane / sphere / AABB ───────────────────────────────────────────────────
// A plane is (normal, d) with signed distance dot(normal, p) + d.

/**
 * @param {Vec3Like} normal
 * @param {number} d
 * @param {Vec3Like} p
 * @returns {number}  0 with fewer than 3 arguments.
 */
bro.math.planeSignedDistance = function(normal, d, p) {};

/**
 * Orthogonal projection of p onto the plane.
 * @param {Vec3Like} normal
 * @param {number} d
 * @param {Vec3Like} p
 * @returns {{x:number,y:number,z:number}|null}
 */
bro.math.planeProject = function(normal, d, p) {};

/**
 * @param {Vec3Like} center
 * @param {number} radius
 * @param {Vec3Like} p
 * @returns {boolean}
 */
bro.math.sphereContains = function(center, radius, p) {};

/**
 * @param {Vec3Like} c0
 * @param {number} r0
 * @param {Vec3Like} c1
 * @param {number} r1
 * @returns {boolean}
 */
bro.math.sphereIntersects = function(c0, r0, c1, r1) {};

/**
 * @param {Vec3Like} boxMin
 * @param {Vec3Like} boxMax
 * @param {Vec3Like} p
 * @returns {boolean}
 */
bro.math.aabbContains = function(boxMin, boxMax, p) {};

/**
 * @param {Vec3Like} minA
 * @param {Vec3Like} maxA
 * @param {Vec3Like} minB
 * @param {Vec3Like} maxB
 * @returns {boolean}
 */
bro.math.aabbIntersects = function(minA, maxA, minB, maxB) {};

/**
 * The box grown to include p.
 * @param {Vec3Like} boxMin
 * @param {Vec3Like} boxMax
 * @param {Vec3Like} p
 * @returns {{min:{x:number,y:number,z:number}, max:{x:number,y:number,z:number}}|null}
 */
bro.math.aabbExpand = function(boxMin, boxMax, p) {};

/**
 * The smallest box holding both boxes.
 * @param {Vec3Like} minA
 * @param {Vec3Like} maxA
 * @param {Vec3Like} minB
 * @param {Vec3Like} maxB
 * @returns {{min:{x:number,y:number,z:number}, max:{x:number,y:number,z:number}}|null}
 */
bro.math.aabbMerge = function(minA, maxA, minB, maxB) {};

// ── Frustum ─────────────────────────────────────────────────────────────────
// A frustum is a flat 24-number array: six planes [nx, ny, nz, d] in the order
// left, right, bottom, top, near, far, normals pointing inward (a point is
// inside when every signed distance is >= 0), each normal unit length so the
// distances are world units.

/**
 * Extract the frustum planes from a 16-element column-major view-projection
 * matrix with OpenGL [-1, 1] clip depth. A non-array is a TypeError.
 *
 * @param {number[]|Float32Array} viewProj
 * @returns {number[]}  24 numbers.
 */
bro.math.frustumFromViewProj = function(viewProj) {};

/**
 * @param {number[]} planes  From frustumFromViewProj.
 * @param {Vec3Like} p
 * @returns {boolean}
 */
bro.math.frustumContainsPoint = function(planes, p) {};

/**
 * Conservative box test (may report true for a box just outside a corner).
 * @param {number[]} planes
 * @param {Vec3Like} boxMin
 * @param {Vec3Like} boxMax
 * @returns {boolean}
 */
bro.math.frustumIntersectsAABB = function(planes, boxMin, boxMax) {};

/**
 * @param {number[]} planes
 * @param {Vec3Like} center
 * @param {number} radius
 * @returns {boolean}
 */
bro.math.frustumIntersectsSphere = function(planes, center, radius) {};

// ── Segments and capsules ───────────────────────────────────────────────────

/**
 * Closest distance between segments p1-q1 and p2-q2.
 * @param {Vec3Like} p1
 * @param {Vec3Like} q1
 * @param {Vec3Like} p2
 * @param {Vec3Like} q2
 * @returns {number}
 */
bro.math.segmentSegmentDistance = function(p1, q1, p2, q2) {};

/**
 * How far two capsules overlap: max(0, rA + rB - core segment distance).
 * @param {Vec3Like} a0
 * @param {Vec3Like} a1
 * @param {number} rA
 * @param {Vec3Like} b0
 * @param {Vec3Like} b1
 * @param {number} rB
 * @returns {number}
 */
bro.math.capsulePenetration = function(a0, a1, rA, b0, b1, rB) {};

/**
 * @param {Vec3Like} a0
 * @param {Vec3Like} a1
 * @param {number} rA
 * @param {Vec3Like} b0
 * @param {Vec3Like} b1
 * @param {number} rB
 * @returns {boolean}
 */
bro.math.capsulesIntersect = function(a0, a1, rA, b0, b1, rB) {};

// ── 2D grid footprint ───────────────────────────────────────────────────────

/**
 * Row-major index row * width + col (not bounds-checked; pair with
 * gridInBounds). Exact for any int col/row. -1 when grid is not an object.
 * @param {MathGrid2D} grid
 * @param {number} col
 * @param {number} row
 * @returns {number}
 */
bro.math.gridIndex2D = function(grid, col, row) {};

/**
 * @param {MathGrid2D} grid
 * @param {number} col
 * @param {number} row
 * @returns {boolean}
 */
bro.math.gridInBounds = function(grid, col, row) {};

/**
 * The cell holding world point p. May be out of range (clamped to +-2^30;
 * NaN goes to the low end).
 * @param {MathGrid2D} grid
 * @param {{x:number,y:number}|number[]} p
 * @returns {{col:number,row:number}|null}
 */
bro.math.gridCellOf = function(grid, p) {};

/**
 * World XY centre of cell (col, row).
 * @param {MathGrid2D} grid
 * @param {number} col
 * @param {number} row
 * @returns {{x:number,y:number}|null}
 */
bro.math.gridCellCenter = function(grid, col, row) {};

