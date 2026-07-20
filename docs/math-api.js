// =====================================================================
// bro.math, math primitives bound from the bromath C++ library
// =====================================================================
//
// Most bromath types (Vec3, Quat, Mat4, AABB, Color) are passed implicitly
// through other APIs as plain JS arrays or objects and are not exposed as
// explicit JS classes. This file documents the bromath surfaces that ARE
// reachable as `bro.math.*`.
//
// Constructors (also registered globally for convenience,
// `bro.math.SpatialHash3D === SpatialHash3D`):
//   SpatialHash3D, uniform-grid 3D spatial index over points and spheres.
//   Rng, deterministic SplitMix64 generator (seeded, reproducible).
//   Smoother, one-pole exponential ramp.
//
// Free functions on bro.math (curves, color, scalar/angle, geometry queries,
// grid/hash) are documented at the bottom of this file.
//
// VECTOR CONVENTION: functions take vectors as either {x,y,z} objects or
// [x,y,z] arrays (2D: {x,y} / [x,y]) and always RETURN them as objects.
// Colors are linear-RGBA {r,g,b,a} (floats); Color8 channels are 0..255 ints.


// -----------------------------------------------------------------------------
// bro.math.SpatialHash3D
// -----------------------------------------------------------------------------
// Uniform-grid 3D spatial hash for fast radius, AABB, and nearest-neighbour
// queries over points and bounding spheres. IDs are caller-assigned int32
// handles; insert/remove are O(1) amortized and queries scan only buckets
// intersecting the query region.
//
// Sphere entries (insertSphere) record their radius alongside the center;
// the grid tracks the maximum radius ever inserted and dilates the cell
// footprint of every query by that amount, so a large sphere whose center
// sits outside the query region is still found.
//
// Mixed point/sphere inserts are fine, point entries behave as zero-radius
// spheres for the purpose of radius queries.

class SpatialHash3D {
  /**
   * @param {number} [cellSize=1] - grid cell width; pick close to the
   *   typical query radius (or the average sphere radius for sphere-heavy
   *   workloads). Too small inflates map churn; too large degenerates to
   *   linear scans of the local cell.
   */
  constructor(cellSize) {}

  /** Reset bucket grid to a new cell size and clear all entries. Returns this. */
  reset(cellSize) {}

  /** Remove all entries (keeps cell size). Returns this. */
  clear() {}

  /** Insert a point with caller-supplied id. Returns this. */
  insert(x, y, z, id) {}

  /**
   * Insert a bounding sphere with caller-supplied id. The grid records
   * the maximum radius inserted so queries dilate accordingly. Returns this.
   */
  insertSphere(x, y, z, radius, id) {}

  /** Remove all entries with the given id. Returns this. */
  remove(id) {}

  /**
   * Return ids whose extent lies within `radius` of (x,y,z). For point
   * entries: dist(center) <= radius. For sphere entries: the sphere's
   * surface comes within `radius`, i.e. dist(center) <= radius + r.
   * @returns {number[]}
   */
  radiusQuery(x, y, z, radius) {}

  /**
   * Return ids whose extent touches the axis-aligned box [(minX,minY,minZ),
   * (maxX,maxY,maxZ)]. Point entries match when their center is inside the
   * box; sphere entries match when their sphere intersects the box.
   * @returns {number[]}
   */
  queryAABB(minX, minY, minZ, maxX, maxY, maxZ) {}

  /**
   * Center-only nearest lookup: id of the entry whose center is closest
   * to (x,y,z), provided that center is within `maxRadius`. Sphere radii
   * are ignored for this query: use radiusQuery for sphere-aware reach.
   * @returns {number} id, or -1 if no entry is within range
   */
  nearest(x, y, z, maxRadius) {}

  /** Number of inserted ids (points + spheres). */
  get size() {}

  /** Current cell size. */
  get cellSize() {}

  /** Maximum sphere radius ever inserted (point inserts contribute 0). */
  get maxRadius() {}
}


// -----------------------------------------------------------------------------
// bro.math.Rng
// -----------------------------------------------------------------------------
// Deterministic SplitMix64 generator carrying its own 64-bit state. Two Rng
// instances seeded with the same value emit identical sequences across runs
// and platforms, use it for reproducible procedural generation, jitter, and
// tests. No global state is touched.
//
//   const rng = new bro.math.Rng(1234);
//   rng.float01();        // 0.xxxx, reproducible for seed 1234
//   rng.range(-1, 1);     // uniform float
//   rng.onUnitSphere();   // { x, y, z } on the unit sphere
//
class Rng {
  /** @param {number} [seed=0] - 64-bit seed (integer). */
  constructor(seed) {}

  /** Re-seed the generator in place. Returns this. */
  reseed(seed) {}

  /** Uniform float in [0, 1). */
  float01() {}

  /** Uniform float in [-1, 1). */
  signed() {}

  /** Uniform float in [lo, hi). */
  range(lo, hi) {}

  /** Uniform integer in [loInclusive, hiInclusive]. */
  int(loInclusive, hiInclusive) {}

  /** 32 fresh bits as an unsigned integer (0 .. 4294967295). */
  uint32() {}

  /** Standard normal sample (mean 0, stddev 1; Box-Muller). */
  normal() {}

  /** 2D gaussian, {x,y}, each ~ N(0, sigma). */
  gaussian2D(sigma) {}

  /** Uniform point inside the unit disc (XY plane) → {x,y}. */
  inUnitDisc() {}

  /** Uniform point inside the unit sphere → {x,y,z}. */
  inUnitSphere() {}

  /** Uniform point on the unit sphere surface → {x,y,z}. */
  onUnitSphere() {}
}


// -----------------------------------------------------------------------------
// bro.math.Smoother
// -----------------------------------------------------------------------------
// One-pole exponential smoother for zipper-free parameter ramps: camera
// follow, UI animation decoupled from frame rate, audio-style automation.
// The time constant is chosen so the value closes ~95% of the gap to the
// target after `timeMs` of ticking at the given tick/sample rate.
//
//   const s = new bro.math.Smoother(200, 60); // 200 ms at 60 Hz
//   s.reset(0).setTarget(1);
//   s.tick();         // advances one tick toward 1, returns new current
//
class Smoother {
  /**
   * @param {number} [timeMs] - smoothing time constant (optional).
   * @param {number} [sampleRate] - tick rate in Hz (required with timeMs).
   */
  constructor(timeMs, sampleRate) {}

  /** Set the time constant for the given tick rate. Returns this. */
  setTime(timeMs, sampleRate) {}

  /** Snap current and target to value (no ramp). Returns this. */
  reset(value) {}

  /** Set the value being chased. Returns this. */
  setTarget(t) {}

  /** Advance one tick; returns the new current value. */
  tick() {}

  /** Advance n ticks at once; returns the new current value. */
  tickN(n) {}

  /** Current smoothed value. */
  get current() {}

  /** Current target value. */
  get target() {}

  /** Smoothing coefficient (derived from setTime). */
  get coeff() {}
}


// =====================================================================
// bro.math free functions
// =====================================================================
//
// Plain functions on bro.math operating on numbers, {x,y,z}/{x,y} vectors
// (or [x,y,z]/[x,y] arrays), and linear {r,g,b,a} colors.

const math = {
  // ---- Curves --------------------------------------------------------------

  /**
   * CSS-style cubic-bezier easing. Control points are (p1x,p1y) and
   * (p2x,p2y); samples the eased y for x in [0,1].
   * @returns {number}
   * @example bro.math.cubicEase(0.25, 0.1, 0.25, 1.0, 0.5) // ~0.8
   */
  cubicEase(p1x, p1y, p2x, p2y, x) {},

  /** 3D cubic Bézier through 4 control points at t. @returns {{x,y,z}} */
  bezier(p0, p1, p2, p3, t) {},

  /** Derivative (tangent) of the cubic Bézier at t. @returns {{x,y,z}} */
  bezierTangent(p0, p1, p2, p3, t) {},

  /**
   * Centripetal Catmull-Rom spline; passes through p1 and p2 with p0/p3 as
   * the surrounding control points. @returns {{x,y,z}}
   */
  catmullRom(p0, p1, p2, p3, t) {},

  /**
   * Cubic Hermite (glTF CubicSpline style). m0 is the in-tangent of p0,
   * m1 the out-tangent of p1. @returns {{x,y,z}}
   */
  hermite(p0, m0, p1, m1, t) {},

  // ---- Color (linear RGBA primary; Color8 = 0..255 sRGB) -------------------

  /** Parse "#RRGGBB" / "#RRGGBBAA" (sRGB) → linear {r,g,b,a}. */
  fromHex(hex) {},

  /** HSV (h in [0,360), s,v in [0,1]) → linear {r,g,b,a}. */
  fromHSV(h, s, v, a = 1) {},

  /** sRGB 8-bit channels (0..255) → linear {r,g,b,a}. */
  fromColor8(r, g, b, a = 255) {},

  /** Linear {r,g,b,a} → sRGB 8-bit {r,g,b,a} (0..255 ints). */
  toColor8(r, g, b, a = 1) {},

  /** Single channel: linear → sRGB. @returns {number} */
  linearToSrgb(c) {},

  /** Single channel: sRGB → linear. @returns {number} */
  srgbToLinear(c) {},

  // ---- Scalar / angle ------------------------------------------------------

  lerp(a, b, t) {},
  clamp(x, lo, hi) {},
  saturate(x) {},            // clamp to [0,1]
  invLerp(a, b, x) {},       // where x lies in [a,b]; 0 when a==b
  remap(x, inMin, inMax, outMin, outMax) {},
  smoothstep(edge0, edge1, x) {},     // 3t^2 - 2t^3
  smootherstep(edge0, edge1, x) {},   // 6t^5 - 15t^4 + 10t^3
  deg2rad(d) {},
  rad2deg(r) {},
  wrapAngle(a) {},           // fold into [-PI, PI]
  wrapAngle2Pi(a) {},        // fold into [0, 2PI)
  angleDelta(from, to) {},   // shortest signed delta, [-PI, PI]
  angleLerp(from, to, t) {}, // shortest-arc interpolation

  // ---- Ray intersection ----------------------------------------------------
  // All return null on a miss, otherwise { t, point:{x,y,z}, normal:{x,y,z} }.
  // Direction need not be unit length; t is in units of `dir`.

  /** @example const hit = bro.math.rayIntersectSphere([0,0,0],[0,0,1],{x:0,y:0,z:5},1) // hit.t === 4 */
  rayIntersectAABB(origin, dir, boxMin, boxMax) {},
  rayIntersectSphere(origin, dir, center, radius) {},
  rayIntersectPlane(origin, dir, normal, d) {},          // plane: dot(n,p)+d=0
  rayIntersectTriangle(origin, dir, v0, v1, v2, backfaceCull = false) {},

  // ---- Plane / sphere / AABB ----------------------------------------------

  /** Signed distance from point to plane dot(normal,p)+d. @returns {number} */
  planeSignedDistance(normal, d, point) {},
  /** Orthogonal projection of point onto the plane. @returns {{x,y,z}} */
  planeProject(normal, d, point) {},

  sphereContains(center, radius, point) {},          // @returns {boolean}
  sphereIntersects(centerA, radiusA, centerB, radiusB) {},

  aabbContains(boxMin, boxMax, point) {},            // @returns {boolean}
  aabbIntersects(minA, maxA, minB, maxB) {},
  /** Grow a box to include a point. @returns {{min:{x,y,z}, max:{x,y,z}}} */
  aabbExpand(boxMin, boxMax, point) {},
  /** Union of two boxes. @returns {{min:{x,y,z}, max:{x,y,z}}} */
  aabbMerge(minA, maxA, minB, maxB) {},

  // ---- Frustum culling -----------------------------------------------------
  // Planes are a flat array of 24 numbers (6 planes × [nx,ny,nz,d], order
  // left,right,bottom,top,near,far). Build once per frame from the
  // column-major view-projection matrix, then test many objects.

  /** @param {number[]} viewProj - 16-element column-major matrix. @returns {number[]} 24 plane components */
  frustumFromViewProj(viewProj) {},
  frustumContainsPoint(planes, point) {},            // @returns {boolean}
  frustumIntersectsAABB(planes, boxMin, boxMax) {},  // conservative
  frustumIntersectsSphere(planes, center, radius) {},

  // ---- Segment / capsule ---------------------------------------------------

  /** Closest distance between segments [p1,q1] and [p2,q2]. @returns {number} */
  segmentSegmentDistance(p1, q1, p2, q2) {},
  /** Overlap depth of two capsules (0 when disjoint). @returns {number} */
  capsulePenetration(a0, a1, radiusA, b0, b1, radiusB) {},
  capsulesIntersect(a0, a1, radiusA, b0, b1, radiusB) {}, // @returns {boolean}

  // ---- Grid (footprint = {origin:{x,y}, cellSize, width, depth}) -----------

  /** Row-major index for cell (col,row). @returns {number} */
  gridIndex2D(footprint, col, row) {},
  gridInBounds(footprint, col, row) {},              // @returns {boolean}
  /** World point → cell. @returns {{col, row}} (may be out of range) */
  gridCellOf(footprint, point) {},
  /** Cell center in world coords. @returns {{x,y}} */
  gridCellCenter(footprint, col, row) {},

  // ---- Hash (deterministic, non-cryptographic; return unsigned 32-bit) -----

  /** FNV-1a 32-bit over a string's UTF-8 bytes. @returns {number} */
  fnv1a32(str, seed) {},
  /** Wang 32-bit integer hash. @returns {number} */
  hashU32(x) {},
  /** Hash a 2D or 3D integer cell coordinate (z optional). @returns {number} */
  cellHash(x, y, z) {},
  /** Hash a {x,y,z} position into a bucket index in [0,bucketCount). @returns {number} */
  positionToCell(point, cellSize, bucketCount) {},
};
