// ── bro.mesh ─────────────────────────────────────────────────────────────────
//
// Geometry: the `Mesh` container, its primitive factories, the mutating
// operators (clean / simplify / subdivide / smooth / CSG / bake), the analysis
// and query surface, `MeshBVH` and `ProgressiveMesh`.
//
// The rest of bromesh lives beside this file:
//   docs/mesh-io-api.js     loaders/savers, Draco, splat clouds, isosurface +
//                           voxel statics, `PolyMesh`, `SDFGraph`, `VoxelChunk`
//   docs/mesh-plants-api.js sweeps, leaf/flower cards, branch trees, leaf
//                           scattering, `CapsuleField`, `LSystem`
//   docs/rigging-api.js     `bro.rigging`: skins, skeletons, poses, clips, IK,
//                           `Rig.autoRig`, glTF rigged assets
//
// Two mount points, one implementation. Every class is a real global — `Mesh`,
// `MeshBVH`, `ProgressiveMesh`, `PolyMesh`, `CapsuleField`, `LSystem`,
// `SDFGraph` — and `bro.mesh` carries those classes plus a *forwarded subset*
// of `Mesh`'s statics. The statics that exist only on the global `Mesh` class
// are `splitByPlane`, `stripify`, `unstripify`, `decode`, `disc`, `dualContour`
// and the `union` / `subtract` / `intersect` aliases; everything else in this
// file is reachable both ways.
//
//   const m = Mesh.sphere(1, 32, 24);
//   m.simplify(0.5).computeNormals();
//   scene.createMesh({ data: m, color: 'red' });
//
// Mutation model: most operators mutate in place and return `this`, so they
// chain. The ones that return a NEW Mesh are called out per method — `clone`,
// `computeFlatNormals`, `convexHull`, the boolean/CSG trio, `splitByPlane`,
// `splitComponents`, `generateLODChain`, `sampleSurface` and every static
// factory. `Mesh.geodesicSphere` and `Mesh.rock` exist only when bromesh was
// built with par_shapes; `Mesh.decodeDraco` / `Mesh.encodeDraco` only with
// Draco (see docs/mesh-io-api.js).
//
// Integer arguments: every count, size, segment, resolution, iteration, lod
// and index argument across bro.mesh and bro.rigging is checked, never
// wrapped or clamped. A non-number is a TypeError; NaN, a fraction or a value
// outside the method's range is a RangeError. The common ceilings: 4096 per
// grid axis (segments, rings, texture sides), 512 per axis of a 3D volume,
// 8 subdivision levels, 65536 iterations, 2^24 generated elements. Iteration
// counts accept 0 as a no-op. Primitive minimums: sphere/torus/cylinder/
// capsule/cone/disk segments >= 3, sphere rings >= 2, capsule rings, cone
// stacks and plane segments >= 1.

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * @typedef {Object} MeshOptions
 * @property {Float32Array} [positions] -  xyz, stride 3. Length must be a multiple of 3.
 * @property {Float32Array} [normals] -  xyz, stride 3; one per vertex.
 * @property {Float32Array} [uvs] -  uv, stride 2; one per vertex.
 * @property {Float32Array} [colors] -  rgba, stride 4; one per vertex.
 * @property {Uint32Array} [indices] -  triangle indices; length must be a multiple of 3.
 */

/**
 *  The axis-aligned bounds `mesh.bounds()` / `bvh.bounds()` return. The scalar
 *  fields and the `min`/`max` arrays describe the same box.
 * @typedef {Object} MeshBounds
 * @property {number} [minX]
 * @property {number} [minY]
 * @property {number} [minZ]
 * @property {number} [maxX]
 * @property {number} [maxY]
 * @property {number} [maxZ]
 * @property {number} [centerX]
 * @property {number} [centerY]
 * @property {number} [centerZ]
 * @property {number} [extentX]
 * @property {number} [extentY]
 * @property {number} [extentZ]
 * @property {Array<number>} [min]
 * @property {Array<number>} [max]
 */

/**
 *  A hit from `raycast` / `raycastAll` / `closestPoint`. A miss is `null`, not
 *  an object with `hit: false`. `triangle` and `triangleIndex` are the same
 *  number, as are `point` and `position`; `uv` is present only when the mesh
 *  carries UVs.
 * @typedef {Object} MeshRayHit
 * @property {boolean} [hit] -  always true on a returned hit.
 * @property {number} [distance]
 * @property {number} [triangle]
 * @property {number} [triangleIndex]
 * @property {Array<number>} [point]
 * @property {Array<number>} [position]
 * @property {Array<number>} [normal]
 * @property {Array<number>} [barycentric] -  [u, v, w] over the hit triangle's corners.
 * @property {Array<number>} [uv]
 */

/**
 *  One intersecting triangle pair from `findSelfIntersections()`.
 * @typedef {Object} MeshTrianglePair
 * @property {number} [triA]
 * @property {number} [triB]
 */

/**
 *  `unwrapUVs()`: the xatlas chart/atlas summary. The UVs themselves land on
 *  `mesh.uvs`.
 * @typedef {Object} MeshUnwrapResult
 * @property {number} [chartCount]
 * @property {number} [atlasWidth]
 * @property {number} [atlasHeight]
 * @property {boolean} [success]
 */

/**
 *  One entry per triangle from `computeUVDistortion()`.
 * @typedef {Object} MeshUVDistortion
 * @property {number} [stretch]
 * @property {number} [areaDistortion]
 * @property {number} [angleDistortion]
 */

/**
 *  `measureUVQuality()`: whole-mesh UV statistics.
 * @typedef {Object} MeshUVQuality
 * @property {number} [avgStretch]
 * @property {number} [maxStretch]
 * @property {number} [avgAreaDistortion]
 * @property {number} [maxAreaDistortion]
 * @property {number} [avgAngleDistortion]
 * @property {number} [maxAngleDistortion]
 * @property {number} [uvSpaceUsage] -  fraction of the unit square the charts cover.
 * @property {number} [triangleCount]
 */

/**
 * @typedef {Object} MeshletParams
 * @property {number} [maxVertices] -  default 64; an integer in [3, 256].
 * @property {number} [maxTriangles] -  default 124; an integer in [1, 512].
 * @property {number} [coneWeight] -  cluster-cone tightness, default 0.5.
 */

/**
 *  Per-meshlet cull bounds: a sphere plus a normal cone.
 * @typedef {Object} MeshletBounds
 * @property {Array<number>} [center]
 * @property {number} [radius]
 * @property {Array<number>} [coneApex]
 * @property {Array<number>} [coneAxis]
 * @property {number} [coneCutoff]
 */

/**
 *  One meshlet. `vertices` indexes the source mesh; `triangles` holds three
 *  bytes per triangle, each indexing `vertices`.
 * @typedef {Object} Meshlet
 * @property {number} [vertexCount]
 * @property {number} [triangleCount]
 * @property {Uint32Array} [vertices]
 * @property {Uint8Array} [triangles]
 * @property {MeshletBounds} [bounds]
 */

/**
 *  What `buildMeshlets` returns: an ARRAY of `Meshlet`, with the flattened GPU
 *  buffers hung off it as extra properties.
 * @typedef {Array<Meshlet>} MeshletGroup
 * @property {number} [meshletCount]
 * @property {Uint32Array} [vertices] -  every meshlet's vertex list, concatenated.
 * @property {Uint8Array} [triangles] -  every meshlet's triangle bytes, concatenated.
 * @property {ArrayBuffer} [meshlets] -  packed 56-byte records: vertexOffset, vertexCount, triangleOffset, triangleCount (u32 x4), center xyz, radius, coneApex xyz, coneAxis xyz, coneCutoff (f32 x11), pad (u32).
 */

/**
 * @typedef {Object} MeshVertexCacheStats
 * @property {number} [verticesTransformed]
 * @property {number} [warpsExecuted]
 * @property {number} [acmr] -  average cache misses per triangle.
 * @property {number} [atvr] -  average transformed vertices per vertex; 1.0 is perfect.
 */

/**
 * @typedef {Object} MeshVertexFetchStats
 * @property {number} [bytesFetched]
 * @property {number} [overfetch] -  bytes fetched / bytes needed.
 */

/**
 * @typedef {Object} MeshOverdrawStats
 * @property {number} [pixelsCovered]
 * @property {number} [pixelsShaded]
 * @property {number} [overdraw]
 */

/**
 *  A baked texture. `pixels` and `data` are the same Float32Array.
 * @typedef {Object} MeshTextureBuffer
 * @property {number} [width]
 * @property {number} [height]
 * @property {number} [channels]
 * @property {Float32Array} [pixels]
 * @property {Float32Array} [data]
 */

/**
 *  V-HACD tuning for `convexDecomposition`. These are the binding's defaults,
 *  which differ from bromesh's C++ struct defaults.
 * @typedef {Object} MeshConvexDecompParams
 * @property {number} [maxHulls] -  default 16.
 * @property {number} [maxVerticesPerHull] -  default 32.
 * @property {number} [resolution] -  voxel resolution, default 100000.
 * @property {number} [minVolumePerHull] -  default 0.0001.
 */

// ── Mesh ─────────────────────────────────────────────────────────────────────

/**
 * Triangle geometry: five parallel attribute streams plus an index buffer.
 * Attribute getters COPY into a fresh typed array, so read once and reuse it
 * rather than re-reading in a loop; the setters validate stride against the
 * current vertex count and throw on a mismatch.
 */
class Mesh {

  /**
   * Build from raw streams, either as an options object or positionally as
   * `(positions, normals, uvs, colors, indices)`. No argument gives an empty
   * mesh.
   *
   * @param {MeshOptions} [opts]
   *
   * @example
   *   const tri = new Mesh({
   *     positions: new Float32Array([0,0,0, 1,0,0, 0,1,0]),
   *     indices: new Uint32Array([0,1,2]),
   *   });
   *   tri.computeNormals();
   */
  constructor(opts) {}

  // --- Attribute streams ----------------------------------------------------

  /** @type {Float32Array} xyz, stride 3. */
  positions;

  /** @type {Float32Array} xyz, stride 3; one per vertex. */
  normals;

  /** @type {Float32Array} uv, stride 2; one per vertex. */
  uvs;

  /** @type {Float32Array} rgba, stride 4; one per vertex. */
  colors;

  /** @type {Uint32Array} triangle indices; setting one out of range throws. */
  indices;

  /** @readonly @type {number} */
  vertexCount;

  /** @readonly @type {number} */
  triangleCount;

  /** @readonly @type {boolean} */
  hasNormals;

  /** @readonly @type {boolean} */
  hasUVs;

  /** @readonly @type {boolean} */
  hasColors;

  /** @readonly @type {boolean} */
  empty;

  // --- Copy, bounds, transforms --------------------------------------------

  /**
   *  A deep copy.
   * @returns {Mesh}
   */
  clone() {}

  /**
   *  Axis-aligned bounds of the current positions. `computeBBox()` is an alias.
   * @returns {MeshBounds}
   */
  bounds() {}

  /** @returns {MeshBounds} */
  computeBBox() {}

  /** Alias of `volume()`. @returns {number} */
  computeVolume() {}

  /** Alias of `surfaceArea()`. @returns {number} */
  computeSurfaceArea() {}

  /** @param {number} dx @param {number} dy @param {number} dz @returns {Mesh} this */
  translate(dx, dy, dz) {}

  /**
   *  Uniform when only `sx` is given.
   * @param {number} sx @param {number} [sy] @param {number} [sz] @returns {Mesh} this
   */
  scale(sx, sy, sz) {}

  /**
   *  Rotate `angle` radians about the axis (ax, ay, az).
   * @param {number} ax @param {number} ay @param {number} az @param {number} angle @returns {Mesh} this
   */
  rotate(ax, ay, az, angle) {}

  /** Recentre on the bounds centre. @returns {Mesh} this */
  center() {}

  /** Centre, then scale so the longest axis measures `size`. @param {number} size @returns {Mesh} this */
  fitToBox(size) {}

  /**
   *  Apply a column-major 4x4. Anything but 16 floats throws.
   * @param {Array<number>} matrix
   * @returns {Mesh} this
   */
  transform(matrix) {}

  /**
   *  Mirror across a principal plane. `axis` is 0 (X), 1 (Y) or 2 (Z);
   *  anything else throws a RangeError.
   * @param {number} axis
   * @returns {Mesh} this
   */
  mirror(axis) {}

  // --- Primitive factories --------------------------------------------------

  /**
   *  Box centred at the origin, given its HALF extents. Passing only `halfW`
   *  makes a cube.
   * @param {number} [halfW=0.5] @param {number} [halfH=halfW] @param {number} [halfD=halfW]
   * @returns {Mesh}
   */
  static box(halfW, halfH, halfD) {}

  /**
   * @param {number} [radius=1] @param {number} [segments=16] @param {number} [rings=12]
   * @returns {Mesh}
   */
  static sphere(radius, segments, rings) {}

  /**
   *  Y-axis cylinder centred at the origin; `halfHeight` is half the total height.
   * @param {number} [radius=0.5] @param {number} [halfHeight=1] @param {number} [segments=16]
   * @returns {Mesh}
   */
  static cylinder(radius, halfHeight, segments) {}

  /**
   * @param {number} [radius=0.5] @param {number} [halfHeight=1] @param {number} [segments=16] @param {number} [rings=8]
   * @returns {Mesh}
   */
  static capsule(radius, halfHeight, segments, rings) {}

  /**
   *  Cone along +Y: base disc at y = 0, apex at y = `height`. Unlike bromesh's
   *  C++ default the base IS capped here unless `capBase` is false.
   * @param {number} [radius=0.5] @param {number} [height=1] @param {number} [segments=16]
   * @param {number} [stacks=4] @param {boolean} [capBase=true]
   * @returns {Mesh}
   */
  static cone(radius, height, segments, stacks, capBase) {}

  /**
   *  Flat grid in the XZ plane, given HALF extents.
   * @param {number} [halfW=1] @param {number} [halfD=halfW] @param {number} [segW=1] @param {number} [segD=1]
   * @returns {Mesh}
   */
  static plane(halfW, halfD, segW, segD) {}

  /**
   *  Torus in the XZ plane.
   * @param {number} [majorRadius=1] @param {number} [minorRadius=0.3]
   * @param {number} [majorSegments=24] @param {number} [minorSegments=12]
   * @returns {Mesh}
   */
  static torus(majorRadius, minorRadius, majorSegments, minorSegments) {}

  /** Unit platonic solid, scaled by `radius` when given. @param {number} [radius] @returns {Mesh} */
  static icosahedron(radius) {}

  /** @param {number} [radius] @returns {Mesh} */
  static dodecahedron(radius) {}

  /** @param {number} [radius] @returns {Mesh} */
  static octahedron(radius) {}

  /** @param {number} [radius] @returns {Mesh} */
  static tetrahedron(radius) {}

  /**
   *  Filled circle in the XZ plane. `Mesh.disc` is the same function under its
   *  bromesh spelling, but only `disk` is forwarded onto `bro.mesh`.
   * @param {number} [radius=1] @param {number} [segments=16]
   * @returns {Mesh}
   */
  static disk(radius, segments) {}

  /** @param {number} [radius=1] @param {number} [segments=16] @returns {Mesh} */
  static disc(radius, segments) {}

  /**
   *  Subdivided icosahedron: 0 = 20 faces, 1 = 80, 2 = 320. Present only in a
   *  par_shapes build.
   * @param {number} [radius=1] @param {number} [subdivisions=2]
   * @returns {Mesh}
   */
  static geodesicSphere(radius, subdivisions) {}

  /**
   *  Noise-displaced sphere. Present only in a par_shapes build. For a scaled
   *  and offset variant in one call see `Mesh.blob` (docs/mesh-plants-api.js).
   * @param {number} [radius=1] @param {number} [seed=1] @param {number} [subdivisions=2]
   * @returns {Mesh}
   */
  static rock(radius, seed, subdivisions) {}

  /**
   * Mesh a row-major height grid in the XZ plane, one vertex per sample.
   * With `border > 0`, `heights` is expected to carry `border` extra rows and
   * columns on every side — full length `(gridW + 2b) * (gridH + 2b)` — used
   * only to compute central-difference normals, so adjacent terrain chunks
   * agree along their shared edge. The geometry still covers `gridW x gridH`.
   *
   * @param {Float32Array} heights
   * @param {number} gridW
   * @param {number} gridH
   * @param {number} [cellSize=1]
   * @param {number} [border=0]
   * @returns {Mesh}
   */
  static heightmapGrid(heights, gridW, gridH, cellSize, border) {}

  /**
   *  Concatenate meshes into one. Takes an array, or the meshes as separate
   *  arguments.
   * @param {Array<Mesh>} meshes
   * @returns {Mesh}
   */
  static merge(meshes) {}

  // --- Normals and tangents -------------------------------------------------

  /** Smooth, area-weighted vertex normals. @returns {Mesh} this */
  computeNormals() {}

  /** Flat per-face normals, splitting shared vertices. @returns {Mesh} a NEW mesh */
  computeFlatNormals() {}

  /**
   * Per-vertex tangents for normal-mapped materials: a flat Float32Array of
   * xyzw per vertex, w carrying the bitangent sign. Needs UVs. This does not
   * touch the mesh — feed the array to your material or vertex buffer.
   *
   * @returns {Float32Array}
   *
   * @example
   *   const m = Mesh.sphere(1, 32, 24);
   *   m.unwrapUVs();
   *   const tangents = m.computeTangents();   // 4 floats per vertex
   */
  computeTangents() {}

  /**
   *  Smooth normals within `angle` degrees, hard across sharper creases.
   * @param {number} [angle=60] -  degrees.
   * @returns {Mesh} this
   */
  computeCreaseNormals(angle) {}

  /** Negate every normal, leaving winding alone. @returns {Mesh} this */
  invertNormals() {}

  /** Reverse triangle winding, leaving normals alone. @returns {Mesh} this */
  flipFaces() {}

  // --- Clean-up -------------------------------------------------------------

  /**
   *  Merge vertices closer than `threshold`.
   * @param {number} [threshold=1e-4]
   * @returns {Mesh} this
   */
  weld(threshold) {}

  /** @param {number} [epsilon=1e-6] @returns {Mesh} this */
  removeDegenerateTriangles(epsilon) {}

  /** @returns {Mesh} this */
  removeDuplicateTriangles() {}

  /**
   *  Fan-fill boundary loops of at most `maxEdges` edges.
   * @param {number} [maxEdges=32]
   * @returns {Mesh} this
   */
  fillHoles(maxEdges) {}

  /** `removeDegenerateTriangles()` then `removeDuplicateTriangles()`. @returns {Mesh} this */
  repair() {}

  // --- Simplification and LOD -----------------------------------------------

  /**
   *  Quadric decimation to `ratio` of the current triangles.
   * @param {number} [ratio=0.5] @param {number} [targetError=1e-3]
   * @returns {Mesh} this
   */
  simplify(ratio, targetError) {}

  /**
   * Quadric decimation that folds UV and normal error into the metric, so UV
   * seams and hard edges survive a heavy reduction that plain `simplify()`
   * would smear. Use it on anything textured.
   *
   * @param {number} [ratio=0.5]
   * @param {number} [targetError=0.01]
   * @param {number} [uvWeight=1]
   * @param {number} [normalWeight=0.5]
   * @returns {Mesh} this
   *
   * @example
   *   const lod1 = mesh.clone().simplifyWithAttributes(0.25, 0.01, 2, 1);
   */
  simplifyWithAttributes(ratio, targetError, uvWeight, normalWeight) {}

  /** @param {number} [count=100] @param {number} [targetError=1e-3] @returns {Mesh} this */
  simplifyToTriangleCount(count, targetError) {}

  /**
   *  One simplified copy per ratio; the source mesh is untouched.
   * @param {Float32Array|Array<number>} ratios
   * @returns {Array<Mesh>}
   */
  generateLODChain(ratios) {}

  // --- Subdivision, smoothing, remeshing ------------------------------------

  /** Loop subdivision (smooths). Each subdivide* takes 0 (no-op) to 8 levels. @param {number} [iterations=1] @returns {Mesh} this */
  subdivideLoop(iterations) {}

  /** Catmull-Clark subdivision (smooths). @param {number} [iterations=1] - 0 to 8. @returns {Mesh} this */
  subdivideCatmullClark(iterations) {}

  /**
   * Plain 1-to-4 midpoint split: no smoothing, so the surface is unchanged and
   * only density rises. This is the subdivision a displacement pass wants —
   * Loop and Catmull-Clark would move the vertices you are about to displace.
   *
   * @param {number} [iterations=1]
   * @returns {Mesh} this
   *
   * @example
   *   const plate = Mesh.plane(2, 2, 8, 8).subdivideMidpoint(2);
   */
  subdivideMidpoint(iterations) {}

  /** Laplacian smoothing; `smoothLaplacian` is the same function. @param {number} [lambda=0.5] @param {number} [iterations=1] @returns {Mesh} this */
  smooth(lambda, iterations) {}

  /** @param {number} [lambda=0.5] @param {number} [iterations=1] @returns {Mesh} this */
  smoothLaplacian(lambda, iterations) {}

  /**
   *  Taubin smoothing: alternating lambda/mu passes, so volume survives.
   * @param {number} [lambda=0.5] @param {number} [mu=-0.53] @param {number} [iterations=1]
   * @returns {Mesh} this
   */
  smoothTaubin(lambda, mu, iterations) {}

  /** `remeshIsotropic(targetEdgeLength, 3)`. @param {number} [targetEdgeLength=0.1] @returns {Mesh} this */
  remesh(targetEdgeLength) {}

  /** @param {number} [targetEdgeLength=0.1] @param {number} [iterations=3] @returns {Mesh} this */
  remeshIsotropic(targetEdgeLength, iterations) {}

  /**
   * Project this mesh's vertices onto `target`. `mode` is `'nearest'` (the
   * default), `'normal'` / `'projectAlongNormal'` (ray-cast along each
   * vertex's own normal) or `'axis'` / `'projectAlongAxis'` (ray-cast along
   * `axis`, default +Y); the numeric enum 0/1/2 also works. `offset` pushes
   * the landing point along the target's normal — useful to float armour just
   * above a body and dodge z-fighting. `maxDistance` 0 means unlimited.
   *
   * @param {Mesh} target
   * @param {string|number} [mode='nearest']
   * @param {number} [maxDistance=0]
   * @param {number} [offset=0]
   * @param {Array<number>} [axis=[0,1,0]]
   * @returns {Mesh} this
   */
  shrinkwrap(target, mode, maxDistance, offset, axis) {}

  // --- Splitting, hulls, booleans -------------------------------------------

  /**
   *  Cut by the plane `n . x = d`; returns `[positiveSide, negativeSide]`.
   * @param {number} [nx=0] @param {number} [ny=1] @param {number} [nz=0] @param {number} [d=0]
   * @returns {Array<Mesh>}
   */
  splitByPlane(nx, ny, nz, d) {}

  /** One mesh per connected component. @returns {Array<Mesh>} */
  splitComponents() {}

  /** @returns {Mesh} a NEW convex hull */
  convexHull() {}

  /**
   * Approximate convex decomposition (V-HACD) into hulls a physics engine can
   * use. Pass the options OBJECT; the positional form
   * `(maxHulls, maxVerticesPerHull, resolution, minVolumePerHull)` is also
   * accepted for callers that predate the object.
   *
   * @param {MeshConvexDecompParams} [params]
   * @returns {Array<Mesh>}
   *
   * @example
   *   const hulls = Mesh.loadOBJ('prop.obj')
   *     .convexDecomposition({ maxHulls: 8, maxVerticesPerHull: 24 });
   *   for (const h of hulls) physics.createBody({ shape: 'convexHull', mesh: h });
   */
  convexDecomposition(params) {}

  /** @param {Mesh} other @returns {Mesh} a NEW mesh */
  booleanUnion(other) {}

  /** @param {Mesh} other @returns {Mesh} a NEW mesh */
  booleanDifference(other) {}

  /** @param {Mesh} other @returns {Mesh} a NEW mesh */
  booleanIntersection(other) {}

  /** Alias of `booleanUnion`. @param {Mesh} other @returns {Mesh} */
  union(other) {}

  /** Alias of `booleanDifference`. @param {Mesh} other @returns {Mesh} */
  subtract(other) {}

  /** Alias of `booleanIntersection`. @param {Mesh} other @returns {Mesh} */
  intersect(other) {}

  // --- Baking ---------------------------------------------------------------
  //
  // The `bake*` methods with no "ToTexture" suffix write into `mesh.colors`;
  // the `*ToTexture` ones need UVs and return a MeshTextureBuffer instead.

  /** @param {number} [rays=64] @param {number} [maxDistance=0] @returns {Mesh} this */
  bakeAmbientOcclusion(rays, maxDistance) {}

  /** @param {number} [scale=1] @returns {Mesh} this */
  bakeCurvature(scale) {}

  /** @param {number} [rays=32] @param {number} [maxDistance=0] @returns {Mesh} this */
  bakeThickness(rays, maxDistance) {}

  /** @param {number} [width=512] @param {number} [height=512] @param {number} [rays=64] @param {number} [maxDistance=0] @returns {MeshTextureBuffer} */
  bakeAOToTexture(width, height, rays, maxDistance) {}

  /** @param {number} [width=512] @param {number} [height=512] @param {number} [scale=1] @returns {MeshTextureBuffer} */
  bakeCurvatureToTexture(width, height, scale) {}

  /** @param {number} [width=512] @param {number} [height=512] @param {number} [rays=32] @param {number} [maxDistance=0] @returns {MeshTextureBuffer} */
  bakeThicknessToTexture(width, height, rays, maxDistance) {}

  /** @param {number} [width=512] @param {number} [height=512] @returns {MeshTextureBuffer} */
  bakeNormalsToTexture(width, height) {}

  /** @param {number} [width=512] @param {number} [height=512] @returns {MeshTextureBuffer} */
  bakePositionToTexture(width, height) {}

  /**
   *  High-to-low normal-map bake: `this` is the low-poly receiver, `high` the
   *  reference.
   * @param {Mesh} high @param {number} [width=512] @param {number} [height=512] @param {number} [maxDistance=0]
   * @returns {MeshTextureBuffer}
   */
  bakeNormalsFromReference(high, width, height, maxDistance) {}

  /**
   * @param {Mesh} high @param {number} [width=512] @param {number} [height=512]
   * @param {number} [rays=64] @param {number} [maxDistance=0]
   * @returns {MeshTextureBuffer}
   */
  bakeAOFromReference(high, width, height, rays, maxDistance) {}

  // --- Measurement and topology --------------------------------------------

  /** @returns {number} */
  surfaceArea() {}

  /** Signed volume; meaningful only on a closed mesh. @returns {number} */
  volume() {}

  /** @returns {boolean} */
  isManifold() {}

  /** Genus from the Euler characteristic, over position-welded vertices. @returns {number} */
  genus() {}

  /** How many edges are shared by a number of faces other than two. @returns {number} */
  nonManifoldEdges() {}

  /**
   *  Per-vertex curvature as an rgba Float32Array (stride 4) — the colours
   *  `bakeCurvature` would write, without touching the mesh.
   *  `computeCurvature` is an alias.
   * @param {number} [scale=1]
   * @returns {Float32Array}
   */
  curvature(scale) {}

  /** @param {number} [scale=1] @returns {Float32Array} */
  computeCurvature(scale) {}

  /** One area per triangle. @returns {Float32Array} */
  triangleAreas() {}

  /**
   *  Area-weighted point cloud over the surface, as a Mesh with positions (and
   *  normals/UVs when the source has them) and no indices.
   * @param {number} [count=100] @param {number} [seed=0] -  0 = non-deterministic.
   * @returns {Mesh}
   */
  sampleSurface(count, seed) {}

  // --- Ray and point queries ------------------------------------------------
  //
  // Every ray method takes either `(origin, direction, maxDistance?)` with
  // vectors as [x,y,z] or {x,y,z}, or the six/seven loose numbers
  // `(ox, oy, oz, dx, dy, dz, maxDistance?)`. `maxDistance` 0 = unlimited.
  // These walk the triangles directly; build a `MeshBVH` for repeated queries.

  /** @param {Array<number>} origin @param {Array<number>} direction @param {number} [maxDistance=0] @returns {MeshRayHit|null} */
  raycast(origin, direction, maxDistance) {}

  /** Every hit along the ray, near to far. @param {Array<number>} origin @param {Array<number>} direction @param {number} [maxDistance=0] @returns {Array<MeshRayHit>} */
  raycastAll(origin, direction, maxDistance) {}

  /** @param {Array<number>} origin @param {Array<number>} direction @param {number} [maxDistance=0] @returns {boolean} */
  raycastTest(origin, direction, maxDistance) {}

  /** @param {Array<number>} point @returns {MeshRayHit|null} */
  closestPoint(point) {}

  // --- Self-intersection ----------------------------------------------------
  //
  // The three together are how a caller validates a mesh before a boolean or a
  // physics bake: ask `hasSelfIntersections()` first, and only pay for
  // `findSelfIntersections()` when you need to show the user where.

  /**
   * @returns {boolean}
   *
   * @example
   *   if (a.hasSelfIntersections()) a.repair();
   *   const cut = a.booleanDifference(b);
   */
  hasSelfIntersections() {}

  /**
   *  Every intersecting triangle pair.
   * @returns {Array<MeshTrianglePair>}
   */
  findSelfIntersections() {}

  /**
   *  Do the two surfaces overlap at all — a cheap broad test before an
   *  expensive boolean.
   * @param {Mesh} other
   * @returns {boolean}
   */
  intersectsMesh(other) {}

  /**
   *  Build an acceleration structure over a SNAPSHOT of this mesh; later edits
   *  do not invalidate it because the BVH holds its own copy.
   * @param {number} [leafSize=8]
   * @returns {MeshBVH}
   */
  buildBVH(leafSize) {}

  // --- UVs ------------------------------------------------------------------

  /**
   *  xatlas chart-based unwrap. Writes `mesh.uvs` and returns the atlas summary.
   * @returns {MeshUnwrapResult}
   */
  unwrapUVs() {}

  /**
   *  Projective UVs. `projection` is `'box'` (default), `'planarXY'`,
   *  `'planarXZ'`, `'planarYZ'`, `'cylindrical'` or `'spherical'`; an unknown
   *  name throws. The second argument is a UV SCALE, not a plane.
   * @param {string} [projection='box'] @param {number} [scale=1]
   * @returns {Mesh} this
   */
  projectUVs(projection, scale) {}

  /**
   *  `'unwrap'` (the default) and `'xatlas'` run `unwrapUVs()`; any other
   *  string is handed to `projectUVs(method, 1)`. Returns whatever the chosen
   *  path returns.
   * @param {string} [method='unwrap']
   * @returns {MeshUnwrapResult|Mesh}
   */
  generateUVs(method) {}

  /** @returns {Array<MeshUVDistortion>} one entry per triangle */
  computeUVDistortion() {}

  /** @returns {MeshUVQuality} */
  measureUVQuality() {}

  // --- GPU-side optimization ------------------------------------------------

  /**
   * Cluster into meshlets for a GPU cluster-cull pass. The returned value is
   * an array of `Meshlet` that ALSO carries the flattened `vertices`,
   * `triangles` and packed `meshlets` buffers as properties, so you can either
   * iterate it or upload it.
   *
   * @param {MeshletParams|number} [opts] -  the options object, or `maxVertices` positionally.
   * @param {number} [maxTriangles] -  only in the positional form.
   * @param {number} [coneWeight] -  only in the positional form.
   * @returns {MeshletGroup}
   *
   * @example
   *   const ms = mesh.buildMeshlets({ maxVertices: 64, maxTriangles: 124 });
   *   console.log(ms.meshletCount, ms[0].bounds.coneCutoff);
   */
  buildMeshlets(opts, maxTriangles, coneWeight) {}

  /** `optimizeVertexCache()` then `optimizeVertexFetch()`. @returns {Mesh} this */
  optimize() {}

  /** @param {number} [cacheSize=16] @returns {MeshVertexCacheStats} */
  analyzeVertexCache(cacheSize) {}

  /** @param {number} [vertexSize=32] -  bytes per vertex. @returns {MeshVertexFetchStats} */
  analyzeVertexFetch(vertexSize) {}

  /** @returns {MeshOverdrawStats} */
  analyzeOverdraw() {}

  /** @returns {Mesh} this */
  optimizeVertexCache() {}

  /** @returns {Mesh} this */
  optimizeVertexFetch() {}

  /**
   *  Reorder for front-to-back coverage; `threshold` is how much ACMR you will
   *  trade for it.
   * @param {number} [threshold=1.05]
   * @returns {Mesh} this
   */
  optimizeOverdraw(threshold) {}

  /** @returns {Mesh} this */
  spatialSortTriangles() {}

  /** @returns {Mesh} this */
  spatialSortVertices() {}

  /**
   *  Position-only index buffer: vertices that share a position collapse, so a
   *  shadow pass draws fewer of them.
   * @returns {Uint32Array}
   */
  generateShadowIndexBuffer() {}

  // `mesh.encode()` / `Mesh.decode()` / `Mesh.stripify()` / `Mesh.unstripify()`
  // are documented with the other serialization entry points in
  // docs/mesh-io-api.js.

  /**
   *  Free-function form of the instance method; returns `[positive, negative]`.
   * @param {Mesh} mesh @param {number} [nx=0] @param {number} [ny=1] @param {number} [nz=0] @param {number} [d=0]
   * @returns {Array<Mesh>}
   */
  static splitByPlane(mesh, nx, ny, nz, d) {}

  /** @param {Mesh} a @param {Mesh} b @returns {Mesh} */
  static booleanUnion(a, b) {}

  /** @param {Mesh} a @param {Mesh} b @returns {Mesh} */
  static booleanDifference(a, b) {}

  /** @param {Mesh} a @param {Mesh} b @returns {Mesh} */
  static booleanIntersection(a, b) {}

  /** @param {Mesh} mesh @returns {Mesh} */
  static convexHull(mesh) {}

  // `mesh.applySkinning()`, `mesh.applyMorphTarget()`, `mesh.saveGLTF()` and
  // `Mesh.loadGLTF()` are installed by the rigging half of bromesh and are
  // documented in docs/rigging-api.js.

}

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
