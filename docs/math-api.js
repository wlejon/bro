// =====================================================================
// bro.math — math primitives bound from the bromath C++ library
// =====================================================================
//
// Most bromath types (Vec3, Quat, Mat4, AABB, Color) are passed implicitly
// through other APIs as plain JS arrays or objects and are not exposed as
// explicit JS classes. This file documents the bromath types that ARE
// surfaced as `bro.math.*` constructors.
//
// Currently:
//   SpatialHash3D — uniform-grid 3D spatial index over points and spheres.
//                   Supports radius / AABB / nearest queries.
//
// The constructors are also registered globally for convenience —
// `bro.math.SpatialHash3D === SpatialHash3D` at the top level.


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
// Mixed point/sphere inserts are fine — point entries behave as zero-radius
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
   * are ignored for this query — use radiusQuery for sphere-aware reach.
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
