// ── bro.mesh: files, fields and voxels ───────────────────────────────────────
//
// The parts of bromesh that read, write or synthesize geometry rather than
// edit it: the file loaders and savers, Gaussian-splat clouds, meshoptimizer
// and Draco compression, the isosurface extractors, the `SDFGraph` CSG
// evaluator, the `PolyMesh` half-edge editor and the `VoxelChunk` grid.
//
// Geometry itself — the `Mesh` class, its primitives, operators and queries —
// is docs/mesh-api.js. Procedural plants are docs/mesh-plants-api.js; rigged
// glTF (`Mesh.loadGLTF`, `mesh.saveGLTF`) is docs/rigging-api.js.
//
// Paths resolve the way `fs.*` resolves them: a relative path is anchored at
// the app directory, not the process's working directory. A file that cannot
// be read gives an empty result rather than throwing — check `mesh.empty` or
// `cloud.count`. Savers return `true` when the file was written.

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 *  A MagicaVoxel grid: `voxels[x + sizeX * (y + sizeY * z)]` is 0 for empty,
 *  else an index into `palette`. Mesh it with `Mesh.greedyMesh`.
 * @typedef {Object} MeshVoxData
 * @property {number} [sizeX]
 * @property {number} [sizeY]
 * @property {number} [sizeZ]
 * @property {Uint8Array} [voxels]
 * @property {Float32Array} [palette] -  rgba x 256, stride 4.
 */

/**
 * A Gaussian splat cloud: the plain object `scene.createGaussianSplat({ cloud })`
 * takes, and the shape `bro.triposplat` produces. Per-splat streams for
 * `count` splats: `positions` xyz, `scales` xyz (linear std-dev), `rotations`
 * xyzw unit quaternion, `opacities` in [0,1], `sh` coefficient-major spherical
 * harmonics with 3 * (shDegree + 1)^2 floats per splat.
 * @typedef {Object} MeshSplatCloud
 * @property {Float32Array} [positions]
 * @property {Float32Array} [scales]
 * @property {Float32Array} [rotations]
 * @property {Float32Array} [opacities]
 * @property {Float32Array} [sh]
 * @property {number} [shDegree]
 * @property {number} [count]
 */

/**
 *  `mesh.encode()` output, and the shape `Mesh.decode()` reads back. The
 *  `has*` flags tell the decoder which attribute streams the blob carries;
 *  keep them with the bytes.
 * @typedef {Object} MeshEncoded
 * @property {Uint8Array} [vertexData]
 * @property {Uint8Array} [indexData]
 * @property {number} [vertexCount]
 * @property {number} [vertexSize]
 * @property {number} [indexCount]
 * @property {boolean} [hasNormals]
 * @property {boolean} [hasUVs]
 * @property {boolean} [hasColors]
 */

/**
 *  One non-standard attribute stream a Draco file carried.
 * @typedef {Object} MeshDracoAttribute
 * @property {string} [type] -  the Draco attribute type name.
 * @property {number} [uniqueId]
 * @property {number} [components]
 * @property {number} [count]
 * @property {string} [kind] -  'float32' | 'int8' | 'uint8' | 'int16' | 'uint16' | 'int32' | 'uint32'
 * @property {Uint8Array} [bytes] -  the raw stream, `kind`-typed.
 */

/**
 * @typedef {Object} MeshDracoDecoded
 * @property {Float32Array} [positions]
 * @property {Float32Array} [normals]
 * @property {Float32Array} [uvs]
 * @property {Float32Array} [colors]
 * @property {Uint32Array} [indices]
 * @property {Mesh} [mesh] -  the same geometry, ready to use.
 * @property {Array<MeshDracoAttribute>} [attributes]
 */

/**
 * @typedef {Object} MeshDracoEncodeOptions
 * @property {number} [positionBits]
 * @property {number} [normalBits]
 * @property {number} [uvBits]
 * @property {number} [colorBits]
 * @property {number} [speed] -  Draco's speed/size dial; `compressionLevel` is read as a fallback spelling.
 * @property {number} [compressionLevel]
 */

/**
 *  Options for `Mesh.reconstruct`.
 * @typedef {Object} MeshReconstructOptions
 * @property {number} [gridResolution] -  voxel grid resolution along the longest axis (default 64).
 * @property {number} [supportRadius] -  influence radius per point; 0 (the default) derives it from point density.
 * @property {number} [isoLevel] -  threshold for surface extraction (default 0.5).
 */

/**
 *  Grid and framing for the `SDFGraph` meshers and the `*SDF` statics.
 *  `bounds` takes either `{ min, max }` (each [x,y,z] or {x,y,z}) or
 *  `{ size }` for the symmetric cube [-size, size]^3.
 * @typedef {Object} MeshSdfMeshOptions
 * @property {number|Array<number>} [dims] -  one number for a cube, or [x, y, z]. Default 64.
 * @property {number} [resolution] -  same as a scalar `dims`.
 * @property {number} [dimX]
 * @property {number} [dimY]
 * @property {number} [dimZ]
 * @property {Object} [bounds] -  default [-2,-2,-2]..[2,2,2].
 * @property {number} [isoLevel] -  default 0.
 * @property {boolean} [closeBoundary] -  cap the surface at the grid wall. Default true. Marching cubes only.
 * @property {boolean} [computeGradients] -  default true. Marching cubes only.
 */

/**
 *  `PolyMesh.tessellate()`: flat-shaded triangles, one normal per face.
 * @typedef {Object} MeshTessellation
 * @property {Float32Array} [positions]
 * @property {Float32Array} [normals]
 * @property {Uint32Array} [indices]
 * @property {Int32Array} [triToFace] -  source face of each triangle.
 * @property {Int32Array} [triToGroup] -  group of each triangle's source face.
 */

/**
 * @typedef {Object} MeshPolyValidation
 * @property {boolean} [valid] -  structurally sound: every face closes, no dangling links.
 * @property {boolean} [isClosed] -  every half-edge has a twin.
 * @property {number} [boundaryHalfEdges]
 * @property {Array<string>} [errors]
 */

/**
 * @typedef {Object} MeshExtrudeFaceResult
 * @property {Int32Array} [dupVerts]
 * @property {Int32Array} [bridgeFaces]
 * @property {Int32Array} [bridgeAdjGroup] -  per bridge face, the group across its boundary edge (-1 at a mesh boundary).
 * @property {number} [backFace] -  the back-face copy that closes the slab, or -1 without one.
 */

/**
 * @typedef {Object} MeshInsetFaceResult
 * @property {number} [innerFace] -  the new interior face, or -1 when the inset was refused.
 * @property {Int32Array} [innerVerts]
 * @property {Int32Array} [bridgeFaces]
 */

// ── File I/O ─────────────────────────────────────────────────────────────────
//
// Loaders are statics on `Mesh`; savers are instance methods. Both are also
// forwarded onto `bro.mesh`.

/**
 * Read a single-mesh file. An unreadable file gives an empty Mesh.
 *
 * @param {string} path
 * @returns {Mesh}
 *
 * @example
 *   const prop = Mesh.loadOBJ('assets/crate.obj');
 *   if (prop.empty) throw new Error('crate.obj missing');
 */
Mesh.loadOBJ = function(path) {};

/** @param {string} path @returns {Mesh} */
Mesh.loadPLY = function(path) {};

/** @param {string} path @returns {Mesh} */
Mesh.loadSTL = function(path) {};

/**
 *  An FBX is a scene, so this gives every mesh it holds, in file order.
 * @param {string} path
 * @returns {Array<Mesh>}
 */
Mesh.loadFBX = function(path) {};

/**
 *  A `.vox` is a voxel grid, not a mesh.
 * @param {string} path
 * @returns {MeshVoxData}
 *
 * @example
 *   const vox = Mesh.loadVOX('assets/ship.vox');
 *   const mesh = Mesh.greedyMesh(vox.voxels, vox.sizeX, vox.sizeY, vox.sizeZ, 0.1);
 */
Mesh.loadVOX = function(path) {};

/**
 *  A Gaussian-splat `.ply` is a cloud, not a mesh; an unreadable file gives an
 *  empty cloud (`count` 0).
 * @param {string} path
 * @returns {MeshSplatCloud}
 */
Mesh.loadSplatPLY = function(path) {};

/**
 *  Write a splat cloud back out. Attribute arrays whose lengths disagree with
 *  the point count throw.
 * @param {string} path
 * @param {MeshSplatCloud} cloud
 * @returns {boolean}
 */
Mesh.saveSplatPLY = function(path, cloud) {};

/**
 * Write the mesh. A relative path is resolved against the app directory by
 * resolving its PARENT directory, so a file written into a directory `fs`
 * just made lands beside it rather than in the process's cwd.
 *
 * @param {string} path
 * @returns {boolean}
 */
Mesh.prototype.saveOBJ = function(path) {};

/** @param {string} path @returns {boolean} */
Mesh.prototype.savePLY = function(path) {};

/** @param {string} path @returns {boolean} */
Mesh.prototype.saveSTL = function(path) {};

// ── Compression ──────────────────────────────────────────────────────────────

/**
 *  meshoptimizer vertex/index compression. Round-trips through `Mesh.decode`,
 *  which needs the `has*` flags to know what streams to rebuild — store the
 *  whole object, not just the two byte arrays.
 * @returns {MeshEncoded}
 */
Mesh.prototype.encode = function() {};

/** @param {MeshEncoded} encoded @returns {Mesh} */
Mesh.decode = function(encoded) {};

/**
 *  Triangle list to strip, with `restartIndex` between strips.
 * @param {Uint32Array} indices
 * @param {number} vertexCount
 * @param {number} [restartIndex=0xFFFFFFFF]
 * @returns {Uint32Array}
 */
Mesh.stripify = function(indices, vertexCount, restartIndex) {};

/** @param {Uint32Array} strip @param {number} [restartIndex=0xFFFFFFFF] @returns {Uint32Array} */
Mesh.unstripify = function(strip, restartIndex) {};

/**
 *  Decode a Draco buffer. Only present when bromesh was built with Draco.
 *  A malformed buffer throws.
 * @param {Uint8Array|ArrayBufferView} bytes
 * @returns {MeshDracoDecoded}
 */
Mesh.decodeDraco = function(bytes) {};

/**
 *  Encode a Mesh, or a plain `{positions, indices, normals?, uvs?, colors?}`
 *  object. Only present in a Draco build.
 * @param {Mesh|Object} meshOrData
 * @param {MeshDracoEncodeOptions} [opts]
 * @returns {Uint8Array}
 */
Mesh.encodeDraco = function(meshOrData, opts) {};

// ── Isosurfaces and voxels ───────────────────────────────────────────────────
//
// The field statics take a flat Float32Array of `dimX * dimY * dimZ` samples
// in x-fastest order. Dimensions that do not match the array length throw.

/**
 *  Marching cubes over a scalar field.
 * @param {Float32Array} values @param {number} dimX @param {number} dimY @param {number} dimZ @param {number} [isoLevel=0]
 * @returns {Mesh}
 */
Mesh.marchingCubes = function(values, dimX, dimY, dimZ, isoLevel) {};

/**
 *  Naive surface nets: fewer, better-shaped triangles than marching cubes,
 *  softer corners.
 * @param {Float32Array} values @param {number} dimX @param {number} dimY @param {number} dimZ @param {number} [isoLevel=0]
 * @returns {Mesh}
 */
Mesh.surfaceNets = function(values, dimX, dimY, dimZ, isoLevel) {};

/**
 *  Dual contouring: keeps sharp features the other two round off.
 *  `Mesh.dualContour` is an alias (global `Mesh` only, not `bro.mesh`).
 * @param {Float32Array} values @param {number} dimX @param {number} dimY @param {number} dimZ @param {number} [isoLevel=0]
 * @returns {Mesh}
 */
Mesh.dualContouring = function(values, dimX, dimY, dimZ, isoLevel) {};

/**
 *  Transvoxel-style LOD chunk: marching cubes over a cubic chunk of
 *  `gridSize` samples per side (`values[z*N*N + y*N + x]`, N = gridSize),
 *  sampled every `2^lod` samples, with the chunk's boundary vertices
 *  snapped to coarser neighbours' grids so adjacent chunks at different
 *  LODs meet without cracks. Pick `gridSize = k * 2^lod + 1` so the cells
 *  reach the far faces. Positions are in sample units times `cellSize`,
 *  whatever the lod, so chunks of any LOD share one coordinate space.
 *
 *  `neighborLods` is the LOD of the chunk across each face, in the order
 *  `[+X, -X, +Y, -Y, +Z, -Z]`. `-1` means no neighbour (a world edge);
 *  an omitted array, a missing entry or a non-number entry all read as -1.
 *  For each face whose neighbour is coarser (`neighborLod > lod`), every
 *  vertex lying on that face has its two in-face coordinates rounded to
 *  the neighbour's grid (a step of `2^neighborLod * cellSize`). A same-LOD
 *  or finer neighbour leaves the face alone (the finer chunk does the
 *  snapping). No transition cells are emitted: the seam is closed by
 *  moving vertices only, so snapped boundary triangles can come out
 *  stretched or degenerate. Normals are recomputed after the snap.
 *
 *  Sign convention is SDF: `value < isoLevel` is inside, normals point out.
 *  A `values` array shorter than `gridSize^3`, or `gridSize < 2`, throws.
 *  `lod` is an integer with `2^lod <= gridSize - 1` (at least one cell), and
 *  each `neighborLods` entry an integer in [-1, 30]; a negative, fractional
 *  or larger value throws a RangeError, a non-number `lod` a TypeError.
 * @example
 *  // lod-0 chunk whose +X neighbour is at lod 1; every other face is open
 *  const chunk = Mesh.transvoxel(field, 17, 0, [1, -1, -1, -1, -1, -1], 0, 0.5);
 * @param {Float32Array} values @param {number} [gridSize=16] @param {number} [lod=0]
 * @param {Array<number>} [neighborLods=[-1,-1,-1,-1,-1,-1]] @param {number} [isoLevel=0] @param {number} [cellSize=1]
 * @returns {Mesh}
 */
Mesh.transvoxel = function(values, gridSize, lod, neighborLods, isoLevel, cellSize) {};

/**
 *  Greedy-mesh a solid voxel grid: runs of equal material merge into single
 *  quads, which is what makes a MagicaVoxel model cheap to draw.
 * @param {Uint8Array} voxels @param {number} [sizeX=16] @param {number} [sizeY=16] @param {number} [sizeZ=16] @param {number} [scale=1]
 * @returns {Mesh}
 */
Mesh.greedyMesh = function(voxels, sizeX, sizeY, sizeZ, scale) {};

/**
 *  Implicit-surface reconstruction of an oriented point cloud — a Mesh with
 *  positions and unit normals; its indices are ignored. `mesh.sampleSurface()`
 *  and `Mesh.loadPLY` on a scan both produce suitable input.
 * @param {Mesh} pointCloud
 * @param {MeshReconstructOptions} [opts]
 * @returns {Mesh}
 */
Mesh.reconstruct = function(pointCloud, opts) {};

/**
 *  Triangulate a simple 2D polygon (`outer` flat x,y,..., CCW for a +Z face)
 *  with optional `holes` (each flat x,y,..., wound CW) into a mesh in the
 *  plane z = `z`. Degenerate input gives an empty Mesh.
 * @param {Array<number>} outer @param {Array<Array<number>>} [holes] @param {number} [z=0]
 * @returns {Mesh}
 */
Mesh.polygon2D = function(outer, holes, z) {};

/**
 *  Triangulate a planar 3D polygon (`outer` flat x,y,z,... on the plane with
 *  unit `normal`). The mesh reuses the input positions and fills normals with
 *  `normal`.
 * @param {Array<number>} outer @param {Array<Array<number>>} [holes] @param {Array<number>} [normal=[0,0,1]]
 * @returns {Mesh}
 */
Mesh.polygon3D = function(outer, holes, normal) {};

// ── SDFGraph ─────────────────────────────────────────────────────────────────

/**
 * A signed-distance CSG graph, compiled and evaluated natively. Every builder
 * method returns an integer NODE ID, not a node object; combinators take those
 * ids. Set a root, then mesh it.
 *
 * `new SDFGraph()` and `Mesh.createSDF()` build the same thing.
 *
 * @example
 *   const g = Mesh.createSDF();
 *   const body = g.roundedBox([1, 0.6, 0.4], 0.15);
 *   const hole = g.translate(g.cylinder(0.25, 2), [0, 0, 0]);
 *   g.setRoot(g.opSubtraction(body, hole));
 *   const mesh = g.surfaceNets({ dims: 96, bounds: { size: 1.5 } });
 */
class SDFGraph {

  constructor() {}

  /** @readonly @type {number} the root node id, or -1 */
  root;

  /** @readonly @type {number} node count */
  size;

  // --- Primitives (each returns a node id) ---------------------------------

  /** @param {number} [radius=1] @returns {number} */
  sphere(radius) {}

  /** @param {Array<number>|number} halfExtents -  [x,y,z]/{x,y,z}, or three loose numbers. @returns {number} */
  box(halfExtents) {}

  /** @param {Array<number>|number} halfExtents @param {number} radius @returns {number} */
  roundedBox(halfExtents, radius) {}

  /** Y-axis cylinder. @param {number} radius @param {number} halfHeight @returns {number} */
  cylinder(radius, halfHeight) {}

  /** @param {Array<number>} a @param {Array<number>} b @param {number} radius @returns {number} */
  capsule(a, b, radius) {}

  /** @param {number} majorRadius @param {number} minorRadius @returns {number} */
  torus(majorRadius, minorRadius) {}

  /** @param {Array<number>} normal @param {number} d @returns {number} */
  plane(normal, d) {}

  // --- Combinators ----------------------------------------------------------
  //
  // Each has a short alias without the `op` prefix: `union`, `intersection`,
  // `subtraction`, `smoothUnion`, `smoothIntersection`, `smoothSubtraction`.

  /** @param {number} a @param {number} b @returns {number} */
  opUnion(a, b) {}

  /** @param {number} a @param {number} b @returns {number} */
  opIntersection(a, b) {}

  /** `a` minus `b`. @param {number} a @param {number} b @returns {number} */
  opSubtraction(a, b) {}

  /** @param {number} a @param {number} b @param {number} k -  blend radius. @returns {number} */
  opSmoothUnion(a, b, k) {}

  /** @param {number} a @param {number} b @param {number} k @returns {number} */
  opSmoothIntersection(a, b, k) {}

  /** @param {number} a @param {number} b @param {number} k @returns {number} */
  opSmoothSubtraction(a, b, k) {}

  // --- Transforms and modifiers ---------------------------------------------

  /** @param {number} child @param {Array<number>} offset @returns {number} */
  translate(child, offset) {}

  /** @param {number} child @param {number} radians @returns {number} */
  rotateY(child, radians) {}

  /** @param {number} child @param {number} s @returns {number} */
  scaleUniform(child, s) {}

  /**
   *  A standalone noise field node, to be fed to `displace`.
   * @param {Array<number>} frequency @param {number} amplitude
   * @returns {number}
   */
  noise3D(frequency, amplitude) {}

  /** Add a noise node's value to a shape's distance. @param {number} child @param {number} noise @returns {number} */
  displace(child, noise) {}

  /** @param {number} node @returns {SDFGraph} this */
  setRoot(node) {}

  /** Content hash, for caching a meshed result across frames. @returns {number} */
  computeHash() {}

  // --- Meshing --------------------------------------------------------------

  /** @param {MeshSdfMeshOptions} [opts] @returns {Mesh} */
  marchingCubes(opts) {}

  /** @param {MeshSdfMeshOptions} [opts] @returns {Mesh} */
  surfaceNets(opts) {}

}

/** @returns {SDFGraph} */
Mesh.createSDF = function() {};

/**
 *  Mesh a graph without holding onto it. The first argument is an `SDFGraph`,
 *  an object carrying one as `{ graph, ...meshOptions }`, or a declarative
 *  shorthand `{ type: 'sphere'|'box'|'torus'|'cylinder', ...params,
 *  ...meshOptions }` — `radius`, `halfExtents`, `majorRadius`/`minorRadius`,
 *  `radius`/`halfHeight` respectively.
 * @param {SDFGraph|Object} graphOrSpec
 * @param {MeshSdfMeshOptions} [opts]
 * @returns {Mesh}
 */
Mesh.marchingCubesSDF = function(graphOrSpec, opts) {};

/**
 * @param {SDFGraph|Object} graphOrSpec
 * @param {MeshSdfMeshOptions} [opts]
 * @returns {Mesh}
 */
Mesh.surfaceNetsSDF = function(graphOrSpec, opts) {};

// ── PolyMesh ─────────────────────────────────────────────────────────────────

/**
 * Half-edge adjacency over N-gon faces: the edit topology a mesh editor keeps
 * beside the triangle Mesh it renders, so a face survives whatever
 * triangulation drew it. Build one from triangles (`fromMeshData` / `fromMesh`,
 * with an optional per-triangle group so coplanar triangles can be merged back
 * into one face with `mergeFacesByGroup`), from a planar polygon, or from N-gon
 * soup; `tessellate()` gives triangles back.
 *
 * Face and vertex indices are stable until `compact()`.
 *
 * @example
 *   const pm = PolyMesh.fromMesh(Mesh.box(1, 1, 1), triGroups);
 *   pm.mergeFacesByGroup();                  // six quads, not twelve triangles
 *   pm.extrudeFace(0, [0, 0.5, 0]);
 *   scene.createMesh({ data: pm.toMesh() });
 */
class PolyMesh {

  /** Empty; the static factories build populated ones. */
  constructor() {}

  /**
   * @param {Float32Array} positions @param {Uint32Array} indices @param {Int32Array} [triToGroup]
   * @returns {PolyMesh}
   */
  static fromMeshData(positions, indices, triToGroup) {}

  /** @param {Mesh} mesh @param {Int32Array} [triToGroup] @returns {PolyMesh} */
  static fromMesh(mesh, triToGroup) {}

  /**
   *  One N-gon from a simple CCW polygon (as seen from +normal).
   * @param {Float32Array} positionsXYZ @param {Array<number>} normal @param {number} [group=0]
   * @returns {PolyMesh}
   */
  static fromPolygon(positionsXYZ, normal, group) {}

  /**
   *  N-gon soup: `polyOffsets` (length F+1) delimits each face's run in
   *  `polyVerts`.
   * @param {Float32Array} positions @param {Uint32Array} polyVerts @param {Uint32Array} polyOffsets @param {Int32Array} [faceGroups]
   * @returns {PolyMesh}
   */
  static fromPolygons(positions, polyVerts, polyOffsets, faceGroups) {}

  /** @readonly @type {number} */
  vertexCount;

  /** @readonly @type {number} */
  halfEdgeCount;

  /** @readonly @type {number} */
  faceCount;

  /** @param {number} faceIdx @returns {number} */
  faceVertexCount(faceIdx) {}

  /** @param {number} faceIdx @returns {Array<number>} */
  faceVertices(faceIdx) {}

  /** @param {number} faceIdx @returns {Array<number>} */
  faceHalfEdges(faceIdx) {}

  /** @param {number} vertexIdx @returns {Array<number>} [x, y, z] */
  getVertex(vertexIdx) {}

  /** @param {number} faceIdx @returns {Array<number>} */
  computeFaceNormal(faceIdx) {}

  /** The face's group tag, or -1. @param {number} faceIdx @returns {number} */
  faceGroup(faceIdx) {}

  /** @param {number} faceIdx @param {number} group */
  setFaceGroup(faceIdx, group) {}

  /** @param {number} groupId @returns {Array<number>} */
  facesInGroup(groupId) {}

  /** @param {number} vertexIdx @returns {boolean} */
  isBoundaryVertex(vertexIdx) {}

  /** @param {number} halfEdgeIdx @returns {boolean} */
  isBoundaryHalfEdge(halfEdgeIdx) {}

  /**
   *  Outer plus hole loops of vertex indices around one face / one group.
   * @param {number} faceIdx
   * @returns {Array<Array<number>>}
   */
  findFaceBoundary(faceIdx) {}

  /** @param {number} groupId @returns {Array<Array<number>>} */
  findGroupBoundary(groupId) {}

  /** @returns {MeshTessellation} */
  tessellate() {}

  /** `tessellate()` as a Mesh: positions, flat normals, indices — no UVs. @returns {Mesh} */
  toMesh() {}

  /** @returns {MeshPolyValidation} */
  validate() {}

  // --- Surgery --------------------------------------------------------------

  /** @param {number} x @param {number} y @param {number} z @returns {number} the new vertex index */
  addVertex(x, y, z) {}

  /**
   *  A face from an ordered vertex loop (3 or more); call `rematchTwins()`
   *  after a batch of these.
   * @param {Array<number>} vertices @param {number} [group=-1]
   * @returns {number} the new face index
   */
  addFace(vertices, group) {}

  /** @param {number} faceIdx */
  deleteFace(faceIdx) {}

  /** @param {number} vertexIdx @param {Array<number>} offset */
  translateVertex(vertexIdx, offset) {}

  /** @param {number} faceIdx @param {Array<number>} offset */
  translateFace(faceIdx, offset) {}

  /**
   *  Push/pull on a closed solid: seam-duplicate vertices move with the face,
   *  so the neighbouring faces stretch instead of tearing.
   * @param {number} faceIdx @param {Array<number>} offset
   */
  translateFaceWithRing(faceIdx, offset) {}

  /**
   *  SketchUp-style extrusion: the face moves by `offset`, a bridge quad is
   *  added per boundary edge, and a back face closes the slab unless
   *  `withBack` is false.
   * @param {number} faceIdx @param {Array<number>} offset @param {boolean} [withBack=true]
   * @param {number} [bridgeGroup=-1] @param {number} [backGroup=-1]
   * @returns {MeshExtrudeFaceResult}
   */
  extrudeFace(faceIdx, offset, withBack, bridgeGroup, backGroup) {}

  /**
   *  Inset toward the centroid by `amount` — a distance, or a ratio in [0,1)
   *  when `asRatio`.
   * @param {number} faceIdx @param {number} amount @param {boolean} [asRatio=false] @param {number} [bridgeGroup=-1]
   * @returns {MeshInsetFaceResult}
   */
  insetFace(faceIdx, amount, asRatio, bridgeGroup) {}

  /**
   *  Split the edge of half-edge `he` at `position` (default: its midpoint);
   *  both adjacent faces must be triangles. Returns the new vertex, or -1.
   * @param {number} he @param {Array<number>} [position]
   * @returns {number}
   */
  splitEdge(he, position) {}

  /** @param {number} he @returns {boolean} */
  flipEdge(he) {}

  /** @param {number} he @param {Array<number>} [position] @returns {boolean} */
  collapseEdge(he, position) {}

  /** Rebuild twin links after a batch of `addFace`. */
  rematchTwins() {}

  /** Merge every run of same-group faces into one N-gon. */
  mergeFacesByGroup() {}

  /** Drop deleted elements and renumber. Invalidates every held index. */
  compact() {}

}

// ── VoxelChunk ───────────────────────────────────────────────────────────────

/**
 * A fixed-size voxel grid with greedy meshing, for editable blocky worlds.
 * Every edit sets a dirty flag, so a renderer can re-mesh only what changed.
 *
 * It is installed as the global `VoxelChunk` and as `bro.rigging.VoxelChunk`
 * (it ships in bromesh's rigging half), NOT on `bro.mesh`.
 *
 * @example
 *   const chunk = new VoxelChunk(32, 32, 32, 0.25);
 *   chunk.fill(0);
 *   chunk.set(4, 2, 4, 1);
 *   if (chunk.isDirty) { node.setMesh(chunk.buildMesh(palette)); chunk.clearDirty(); }
 */
class VoxelChunk {

  /**
   * @param {number} [sizeX=16] @param {number} [sizeY=16] @param {number} [sizeZ=16] @param {number} [cellSize=1]
   */
  constructor(sizeX, sizeY, sizeZ, cellSize) {}

  /** @readonly @type {number} */
  sizeX;

  /** @readonly @type {number} */
  sizeY;

  /** @readonly @type {number} */
  sizeZ;

  /** @readonly @type {number} */
  cellSize;

  /** @type {boolean} writable: assigning true marks dirty, false clears. */
  isDirty;

  /** Set a material index (0 = empty); `setVoxel` is an alias. @param {number} x @param {number} y @param {number} z @param {number} material @returns {VoxelChunk} this */
  set(x, y, z, material) {}

  /** @param {number} x @param {number} y @param {number} z @param {number} material @returns {VoxelChunk} this */
  setVoxel(x, y, z, material) {}

  /** 0 when out of bounds; `getVoxel` is an alias. @param {number} x @param {number} y @param {number} z @returns {number} */
  get(x, y, z) {}

  /** @param {number} x @param {number} y @param {number} z @returns {number} */
  getVoxel(x, y, z) {}

  /** @param {number} material @returns {VoxelChunk} this */
  fill(material) {}

  /** @returns {VoxelChunk} this */
  markDirty() {}

  /** @returns {VoxelChunk} this */
  clearDirty() {}

  /** A copy of the whole grid, x-fastest. @returns {Uint8Array} */
  data() {}

  /** Bulk copy in, clipped to the grid size; marks dirty. @param {Uint8Array} bytes @returns {VoxelChunk} this */
  setData(bytes) {}

  /**
   *  Greedy-mesh the grid. `palette` is rgba per material (stride 4); without
   *  one the mesh carries no colours. `toMesh()` forwards here.
   * @param {Float32Array} [palette] @param {number} [paletteCount=palette.length / 4]
   * @returns {Mesh}
   */
  buildMesh(palette, paletteCount) {}

  /** @returns {Mesh} */
  toMesh() {}

}
