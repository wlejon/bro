// ── bro.mesh: MeshBVH and ProgressiveMesh ───────────────────────────────────
//
// The two classes built over a `Mesh` (docs/mesh-api.js) for repeated use:
// `MeshBVH`, a bounding-volume hierarchy for ray and closest-point queries,
// and `ProgressiveMesh`, an edge-collapse sequence any level of detail is
// pulled from. Both are real globals and on `bro.mesh`. `MeshBounds` and
// `MeshRayHit` are docs/mesh-api.js's.

// ── MeshBVH ──────────────────────────────────────────────────────────────────

/**
 * A bounding-volume hierarchy over a COPY of the mesh it was built from, so
 * later edits to that mesh leave it valid (and stale — rebuild after an edit
 * you care about). Build one whenever a mesh takes more than a handful of ray
 * queries.
 *
 * Every query optionally takes a Mesh as its first argument: pass one to run
 * the traversal against different geometry with the same topology (a posed
 * copy of the bind mesh, say) instead of the snapshot.
 *
 * @example
 *   const bvh = new MeshBVH(terrain, 8);          // or terrain.buildBVH()
 *   const hit = bvh.raycast([x, 100, z], [0, -1, 0]);
 *   if (hit) place(hit.point, hit.normal);
 */
class MeshBVH {

  /**
   * @param {Mesh} mesh
   * @param {number} [leafSize=8]
   */
  constructor(mesh, leafSize) {}

  /** @readonly @type {boolean} */
  empty;

  /** @readonly @type {number} */
  triangleCount;

  /** @readonly @type {number} */
  nodeCount;

  /** @returns {MeshBounds} */
  bounds() {}

  /**
   * @param {Array<number>} origin @param {Array<number>} direction @param {number} [maxDistance=0]
   * @returns {MeshRayHit|null}
   */
  raycast(origin, direction, maxDistance) {}

  /**
   * @param {Array<number>} origin @param {Array<number>} direction @param {number} [maxDistance=0]
   * @returns {boolean}
   */
  raycastTest(origin, direction, maxDistance) {}

  /** @param {Array<number>} point @returns {MeshRayHit|null} */
  closestPoint(point) {}

}

// ── ProgressiveMesh ──────────────────────────────────────────────────────────

/**
 * A pre-computed edge-collapse sequence: build it once, then pull any level of
 * detail out of it in constant time. Cheaper than re-running `simplify()` per
 * LOD, and the levels are nested, so they pop less.
 *
 * @example
 *   const pm = new ProgressiveMesh(hero);
 *   const near = pm.atRatio(1.0);
 *   const far  = pm.atTriangleCount(500);
 *   fs.writeFileSync('hero.pm', pm.serialize());
 */
class ProgressiveMesh {

  /** @param {Mesh} mesh */
  constructor(mesh) {}

  /** @readonly @type {number} triangles at full detail */
  maxTriangles;

  /** @readonly @type {number} triangles once every collapse is applied */
  minTriangles;

  /** @readonly @type {number} edge collapses in the recorded sequence */
  collapseCount;

  /** @param {number} ratio -  0..1 of `maxTriangles`. @returns {Mesh} */
  atRatio(ratio) {}

  /** @param {number} count @returns {Mesh} */
  atTriangleCount(count) {}

  /** @returns {Uint8Array} */
  serialize() {}

  /** @param {Uint8Array} bytes @returns {ProgressiveMesh} */
  static deserialize(bytes) {}

}
