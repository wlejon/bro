// =============================================================================
// bro Mesh API Reference
// =============================================================================
//
// The bromesh C++ library is exposed via these classes and namespaces:
//
//   Mesh             — geometry container plus primitive factories, I/O, CSG,
//                      isosurface extraction, simplification, baking, UV, and
//                      analysis. Also: applySkinning, applyMorphTarget.
//   MeshBVH          — bounding-volume hierarchy for accelerated ray queries.
//   ProgressiveMesh  — pre-computed edge-collapse sequence for streaming LOD.
//
//   Skeleton         — bones + sockets + bindPose.
//   Pose             — flat array of local TRS per bone (stride 10).
//                      computeWorld/SkinningMatrices, socketWorld, blend.
//   Animation        — keyframed channels; evaluate, evaluateInto, retarget.
//   SkinData         — per-vertex bone weights/indices + inverse-bind matrices;
//                      normalize, validate, transfer.
//   RigSpec          — opaque template (humanoid / quadruped / hexapod /
//                      octopod, or custom JSON).
//   VoxelChunk       — fixed-size voxel grid with greedy meshing.
//
//   IK.*             — twoBone, FABRIK, lookAt solvers (mutate a Pose).
//   Rig.*            — spec / specFromJSON / detectHumanoid / detectQuadruped
//                      / missingLandmarks / fitSkeleton / autoRig /
//                      generateLocomotionCycle.
//
// Mesh objects hold geometry data and can be used independently or fed into
// the scene graph for rendering.
//
// Basic usage:
//   const box = Mesh.box();
//   console.log(box.vertexCount, box.triangleCount);
//
// With the scene graph:
//   const scene = canvas.getContext('scene');
//   const mesh = Mesh.sphere(1, 32, 24);
//   mesh.simplify(0.5);
//   const node = scene.createMesh({ data: mesh, color: 'red' });
//
// =============================================================================


// -----------------------------------------------------------------------------
// Mesh
// -----------------------------------------------------------------------------
// Global class wrapping bromesh::MeshData.

class Mesh {

  // --- Constructor ----------------------------------------------------------

  /**
   * Create a new Mesh, optionally from raw typed array data.
   * @param {Object} [opts]
   * @param {Float32Array} [opts.positions] - xyz, stride 3
   * @param {Float32Array} [opts.normals] - xyz, stride 3
   * @param {Float32Array} [opts.uvs] - uv, stride 2
   * @param {Float32Array} [opts.colors] - rgba, stride 4
   * @param {Uint32Array} [opts.indices] - triangle indices
   */
  constructor(opts) {}


  // --- Properties -----------------------------------------------------------

  /** Vertex positions as Float32Array (xyz, stride 3). Getter copies; setter replaces. */
  get positions() {}
  set positions(arr) {}

  /** Vertex normals as Float32Array (xyz, stride 3). */
  get normals() {}
  set normals(arr) {}

  /** UV coordinates as Float32Array (uv, stride 2). */
  get uvs() {}
  set uvs(arr) {}

  /** Vertex colors as Float32Array (rgba, stride 4). */
  get colors() {}
  set colors(arr) {}

  /** Triangle indices as Uint32Array. */
  get indices() {}
  set indices(arr) {}

  /** Number of vertices (read-only). */
  get vertexCount() {}

  /** Number of triangles (read-only). */
  get triangleCount() {}

  /** Whether normals are present (read-only). */
  get hasNormals() {}

  /** Whether UVs are present (read-only). */
  get hasUVs() {}

  /** Whether vertex colors are present (read-only). */
  get hasColors() {}

  /** Whether the mesh has no geometry (read-only). */
  get empty() {}


  // --- Clone ----------------------------------------------------------------

  /** @returns {Mesh} deep copy */
  clone() {}


  // --- Static: Primitives (parametric) --------------------------------------

  /**
   * Axis-aligned box centered at origin.
   * @param {number} [halfW=0.5]
   * @param {number} [halfH=0.5]
   * @param {number} [halfD=0.5]
   * @returns {Mesh}
   */
  static box(halfW, halfH, halfD) {}

  /**
   * UV sphere centered at origin.
   * @param {number} [radius=0.5]
   * @param {number} [segments=16]
   * @param {number} [rings=12]
   * @returns {Mesh}
   */
  static sphere(radius, segments, rings) {}

  /**
   * Cylinder along Y axis, centered at origin.
   * @param {number} [radius=0.5]
   * @param {number} [halfHeight=0.5]
   * @param {number} [segments=16]
   * @returns {Mesh}
   */
  static cylinder(radius, halfHeight, segments) {}

  /**
   * Capsule along Y axis, centered at origin.
   * @param {number} [radius=0.5]
   * @param {number} [halfHeight=0.5]
   * @param {number} [segments=16]
   * @param {number} [rings=8]
   * @returns {Mesh}
   */
  static capsule(radius, halfHeight, segments, rings) {}

  /**
   * Flat plane in XZ plane, centered at origin, facing +Y.
   * @param {number} [halfW=5]
   * @param {number} [halfD=5]
   * @param {number} [subdivX=1]
   * @param {number} [subdivZ=1]
   * @returns {Mesh}
   */
  static plane(halfW, halfD, subdivX, subdivZ) {}

  /**
   * Torus centered at origin, lying in XZ plane.
   * @param {number} [majorRadius=1]
   * @param {number} [minorRadius=0.3]
   * @param {number} [majorSegments=24]
   * @param {number} [minorSegments=12]
   * @returns {Mesh}
   */
  static torus(majorRadius, minorRadius, majorSegments, minorSegments) {}

  /**
   * Mesh from a heightmap grid.
   * @param {Float32Array} heights - row-major Y values
   * @param {number} gridW - grid width
   * @param {number} gridH - grid height
   * @param {number} [cellSize=1]
   * @returns {Mesh}
   */
  static heightmapGrid(heights, gridW, gridH, cellSize) {}


  // --- Static: Primitives (procedural / Platonic) ---------------------------

  /**
   * Geodesic sphere by subdividing an icosahedron.
   * Triangle count = 20 * 4^nsubdivisions.
   * @param {number} [radius=0.5]
   * @param {number} [nsubdivisions=2]
   * @returns {Mesh}
   */
  static geodesicSphere(radius, nsubdivisions) {}

  /** Unit icosahedron (20 triangles). @returns {Mesh} */
  static icosahedron() {}

  /** Unit dodecahedron. @returns {Mesh} */
  static dodecahedron() {}

  /** Unit octahedron (8 triangles). @returns {Mesh} */
  static octahedron() {}

  /** Unit tetrahedron (4 triangles). @returns {Mesh} */
  static tetrahedron() {}

  /**
   * Cone along Y axis.
   * @param {number} [radius=0.5]
   * @param {number} [height=1]
   * @param {number} [slices=16]
   * @param {number} [stacks=4]
   * @returns {Mesh}
   */
  static cone(radius, height, slices, stacks) {}

  /**
   * Disc (filled circle) in the XZ plane.
   * @param {number} [radius=0.5]
   * @param {number} [slices=16]
   * @returns {Mesh}
   */
  static disc(radius, slices) {}

  /**
   * Procedural rock — displaced icosphere.
   * @param {number} [radius=0.5]
   * @param {number} [seed=42]
   * @param {number} [nsubdivisions=2]
   * @returns {Mesh}
   */
  static rock(radius, seed, nsubdivisions) {}

  /**
   * Trefoil knot.
   * @param {number} [radius=1]
   * @param {number} [slices=64]
   * @param {number} [stacks=16]
   * @returns {Mesh}
   */
  static trefoilKnot(radius, slices, stacks) {}

  /**
   * Klein bottle.
   * @param {number} [slices=32]
   * @param {number} [stacks=16]
   * @returns {Mesh}
   */
  static kleinBottle(slices, stacks) {}


  // --- Static: CSG ----------------------------------------------------------

  /** Boolean union of two meshes. @returns {Mesh} */
  static union(a, b) {}

  /** Boolean difference (a - b). @returns {Mesh} */
  static subtract(a, b) {}

  /** Boolean intersection of two meshes. @returns {Mesh} */
  static intersect(a, b) {}

  /**
   * Split a mesh by a plane.
   * @param {Mesh} mesh
   * @param {number} nx - plane normal X
   * @param {number} ny - plane normal Y
   * @param {number} nz - plane normal Z
   * @param {number} offset - plane distance from origin
   * @returns {Mesh[]} [front, back]
   */
  static splitByPlane(mesh, nx, ny, nz, offset) {}

  /**
   * Merge multiple meshes into one. Index buffers are concatenated and
   * rebased; attributes are carried through.
   * @param {Mesh[]} meshes
   * @returns {Mesh}
   */
  static merge(meshes) {}


  // --- Static: Isosurface / voxel -------------------------------------------

  /**
   * Marching cubes isosurface extraction.
   * @param {Float32Array} field - scalar field, row-major
   * @param {number} gridX
   * @param {number} gridY
   * @param {number} gridZ
   * @param {number} [isoLevel=0]
   * @param {number} [cellSize=1]
   * @returns {Mesh}
   */
  static marchingCubes(field, gridX, gridY, gridZ, isoLevel, cellSize) {}

  /**
   * Dual contouring isosurface extraction.
   * @param {Float32Array} field
   * @param {number} gridX
   * @param {number} gridY
   * @param {number} gridZ
   * @param {number} [isoLevel=0]
   * @param {number} [cellSize=1]
   * @returns {Mesh}
   */
  static dualContour(field, gridX, gridY, gridZ, isoLevel, cellSize) {}

  /**
   * Naive surface nets — smoother than marching cubes with shared vertices.
   * @param {Float32Array} field
   * @param {number} gridX
   * @param {number} gridY
   * @param {number} gridZ
   * @param {number} [isoLevel=0]
   * @param {number} [cellSize=1]
   * @returns {Mesh}
   */
  static surfaceNets(field, gridX, gridY, gridZ, isoLevel, cellSize) {}

  /**
   * Transvoxel for seamless LOD transitions between voxel chunks.
   * @param {Float32Array} field - scalar values for this chunk
   * @param {number} gridSize - cubic grid dimension
   * @param {number} lod - LOD level of this chunk (0 = finest)
   * @param {number[]} neighborLods - [+X, -X, +Y, -Y, +Z, -Z] (-1 = no neighbor)
   * @param {number} [isoLevel=0]
   * @param {number} [cellSize=1]
   * @returns {Mesh}
   */
  static transvoxel(field, gridSize, lod, neighborLods, isoLevel, cellSize) {}

  /**
   * Greedy meshing for voxel data.
   * @param {Uint8Array} voxels - 3D grid, 0 = empty, nonzero = palette index
   * @param {number} gridX
   * @param {number} gridY
   * @param {number} gridZ
   * @param {number} [cellSize=1]
   * @param {Float32Array} [palette] - RGBA per material
   * @param {number} [paletteCount]
   * @param {number} [filterMaterial=-1] - render only this material id
   * @returns {Mesh}
   */
  static greedyMesh(voxels, gridX, gridY, gridZ, cellSize, palette, paletteCount, filterMaterial) {}


  // --- Static: File I/O (load) ----------------------------------------------

  /** Load a glTF/GLB file. @returns {{ meshes: Mesh[] }} */
  static loadGLTF(path) {}

  /** Load a Wavefront OBJ file. @returns {Mesh} */
  static loadOBJ(path) {}

  /** Load an FBX file. @returns {Mesh[]} */
  static loadFBX(path) {}

  /** Load a PLY file. @returns {Mesh} */
  static loadPLY(path) {}

  /** Load a binary STL file. @returns {Mesh} */
  static loadSTL(path) {}

  /**
   * Load a MagicaVoxel VOX file.
   * @returns {{ sizeX: number, sizeY: number, sizeZ: number,
   *             voxels: Uint8Array, palette: Float32Array }}
   */
  static loadVOX(path) {}


  // --- Static: Reconstruction ----------------------------------------------

  /**
   * Reconstruct a mesh from a point cloud.
   * @param {Mesh} pointCloud - mesh with positions (and optionally normals)
   * @param {Object} [params]
   * @param {number} [params.gridResolution=64]
   * @param {number} [params.supportRadius=0] - 0 = auto
   * @param {number} [params.isoLevel=0.5]
   * @returns {Mesh}
   */
  static reconstruct(pointCloud, params) {}


  // --- Static: Encoding / stripification ------------------------------------

  /**
   * Decode a previously encoded mesh back to a Mesh.
   * The argument is an object produced by mesh.encode().
   * @param {{vertexData: Uint8Array, indexData: Uint8Array,
   *          vertexCount: number, vertexSize: number, indexCount: number,
   *          hasNormals: boolean, hasUVs: boolean, hasColors: boolean}} encoded
   * @returns {Mesh}
   */
  static decode(encoded) {}

  /**
   * Convert a triangle list index buffer to a triangle strip.
   * @param {Uint32Array} indices
   * @param {number} vertexCount
   * @param {number} [restartIndex=0xFFFFFFFF]
   * @returns {Uint32Array}
   */
  static stripify(indices, vertexCount, restartIndex) {}

  /**
   * Convert a triangle strip back to a triangle list.
   * @param {Uint32Array} strip
   * @param {number} [restartIndex=0xFFFFFFFF]
   * @returns {Uint32Array}
   */
  static unstripify(strip, restartIndex) {}


  // --- Transform (mutating, return this) ------------------------------------

  /** Translate all vertices. @returns {Mesh} this */
  translate(dx, dy, dz) {}

  /**
   * Scale all vertices. One arg = uniform, three = per-axis.
   * Non-uniform scale triggers normal recomputation.
   * @returns {Mesh} this
   */
  scale(sx, sy, sz) {}

  /** Rotate around axis by angle (radians). @returns {Mesh} this */
  rotate(axisX, axisY, axisZ, angleRadians) {}

  /** Center at origin. @returns {Mesh} this */
  center() {}

  /** Mirror across an axis (0=X, 1=Y, 2=Z). Flips winding. @returns {Mesh} this */
  mirror(axis) {}

  /** Apply a 4x4 column-major matrix. @param {Float32Array} matrix @returns {Mesh} this */
  transform(matrix) {}


  // --- Normals / tangents ---------------------------------------------------

  /** Recompute smooth normals in-place. @returns {Mesh} this */
  computeNormals() {}

  /** Compute flat normals (duplicates vertices). @returns {Mesh} new mesh */
  computeFlatNormals() {}

  /**
   * Recompute normals with hard creases above an angle threshold.
   * @param {number} [angleThresholdDeg=30]
   * @returns {Mesh} this
   */
  computeCreaseNormals(angleThresholdDeg) {}

  /** Compute tangents. @returns {Float32Array} 4 floats per vertex */
  computeTangents() {}


  // --- Welding / repair (mutating, return this) -----------------------------

  /** Merge coincident vertices. @param {number} [epsilon=1e-5] @returns {Mesh} this */
  weld(epsilon) {}

  /** Remove zero-area triangles. @returns {Mesh} this */
  removeDegenerateTriangles(areaEpsilon) {}

  /** Remove duplicate triangles (same three verts). @returns {Mesh} this */
  removeDuplicateTriangles() {}

  /**
   * Fill simple boundary loops with a fan. Only fills holes up to maxEdges.
   * @param {number} [maxEdges=64]
   * @returns {Mesh} this
   */
  fillHoles(maxEdges) {}

  /**
   * Split a mesh into its connected components.
   * @returns {Mesh[]}
   */
  splitComponents() {}

  /**
   * Project the vertices of this mesh onto another mesh's surface.
   * @param {Mesh} target
   * @param {string} [mode="nearest"] - "nearest"|"projectAlongNormal"|"projectAlongAxis"
   * @param {number} [maxDistance=0] - 0 = unlimited
   * @param {number} [offset=0] - push along the target's normal after projection
   * @param {number[]} [axis] - only used for "projectAlongAxis"
   * @returns {Mesh} this
   */
  shrinkwrap(target, mode, maxDistance, offset, axis) {}


  // --- Simplification (mutating, return this) -------------------------------

  /** @param {number} targetRatio 0.0-1.0 @returns {Mesh} this */
  simplify(targetRatio, targetError) {}

  /** @returns {Mesh} this */
  simplifyWithAttributes(targetRatio, targetError, uvWeight, normalWeight) {}

  /** @returns {Mesh} this */
  simplifyToTriangleCount(targetTriangles, targetError) {}

  /** @param {Float32Array} ratios - e.g. [0.5, 0.25, 0.1] @returns {Mesh[]} */
  generateLODChain(ratios) {}


  // --- Subdivision (mutating, return this) ----------------------------------

  /** Loop subdivision. @returns {Mesh} this */
  subdivideLoop(iterations) {}

  /** Catmull–Clark subdivision. @returns {Mesh} this */
  subdivideCatmullClark(iterations) {}

  /** Midpoint subdivision. @returns {Mesh} this */
  subdivideMidpoint(iterations) {}


  // --- Smoothing / remeshing (mutating, return this) ------------------------

  /** Laplacian smoothing. @returns {Mesh} this */
  smoothLaplacian(lambda, iterations) {}

  /** Volume-preserving Taubin smoothing. @returns {Mesh} this */
  smoothTaubin(lambda, mu, iterations) {}

  /**
   * Isotropic remeshing — split long edges, collapse short ones, relax valence.
   * @param {number} [targetEdgeLength=0] - 0 = auto
   * @param {number} [iterations=5]
   * @returns {Mesh} this
   */
  remeshIsotropic(targetEdgeLength, iterations) {}


  // --- GPU optimization (mutating) ------------------------------------------

  /** Reorder indices for GPU vertex cache. @returns {Mesh} this */
  optimizeVertexCache() {}

  /** Reorder vertices for sequential access. @returns {Mesh} this */
  optimizeVertexFetch() {}

  /** Reorder for reduced overdraw. @returns {Mesh} this */
  optimizeOverdraw(threshold) {}

  /** Sort triangles by spatial locality. @returns {Mesh} this */
  spatialSortTriangles() {}

  /** Sort vertices by a space-filling curve. @returns {Mesh} this */
  spatialSortVertices() {}

  /**
   * Index buffer optimized for position-only rendering (Z-prepass, shadows).
   * @returns {Uint32Array}
   */
  generateShadowIndexBuffer() {}


  // --- Meshlets / encoding --------------------------------------------------

  /**
   * Cluster the mesh into GPU-friendly meshlets for mesh-shader pipelines.
   * @param {Object} [params]
   * @param {number} [params.maxVertices=64]
   * @param {number} [params.maxTriangles=124]
   * @param {number} [params.coneWeight=0.5]
   * @returns {Array<{ vertices: Uint32Array, triangles: Uint8Array,
   *                   bounds: { center: number[], radius: number,
   *                             coneApex: number[], coneAxis: number[], coneCutoff: number } }>}
   */
  buildMeshlets(params) {}

  /**
   * Encode mesh for compact streaming/storage. Round-trip via Mesh.decode().
   * @returns {{ vertexData: Uint8Array, indexData: Uint8Array,
   *            vertexCount: number, vertexSize: number, indexCount: number,
   *            hasNormals: boolean, hasUVs: boolean, hasColors: boolean }}
   */
  encode() {}


  // --- Cache / overdraw analysis --------------------------------------------

  /** @returns {{ verticesTransformed, warpsExecuted, acmr, atvr }} */
  analyzeVertexCache(cacheSize) {}

  /** @returns {{ bytesFetched, overfetch }} */
  analyzeVertexFetch(vertexSize) {}

  /** @returns {{ pixelsCovered, pixelsShaded, overdraw }} */
  analyzeOverdraw() {}


  // --- Geometric analysis ---------------------------------------------------

  /** @returns {{ min: number[], max: number[], centerX, centerY, centerZ, extentX, extentY, extentZ }} */
  computeBBox() {}

  /** @returns {boolean} */
  isManifold() {}

  /** @returns {number} signed volume (manifold meshes only) */
  computeVolume() {}

  /** Total surface area. @returns {number} */
  surfaceArea() {}

  /** Per-triangle areas. @returns {Float32Array} */
  triangleAreas() {}

  /**
   * Sample random points uniformly on the surface.
   * @param {number} numSamples
   * @param {number} [seed=0] - 0 = non-deterministic
   * @returns {Mesh} point cloud (positions + optional normals/UVs; no indices)
   */
  sampleSurface(numSamples, seed) {}

  /**
   * Cast a ray against the mesh.
   * @returns {?{ distance, position: number[], normal: number[], triangleIndex, baryU, baryV, baryW }}
   */
  raycast(origin, direction, maxDistance) {}

  /** @returns {Object[]} all hits */
  raycastAll(origin, direction, maxDistance) {}

  /** @returns {boolean} */
  raycastTest(origin, direction, maxDistance) {}

  /** @returns {?Object} closest point on the surface */
  closestPoint(point) {}

  /** @returns {boolean} */
  hasSelfIntersections() {}

  /** @returns {{ triA: number, triB: number }[]} */
  findSelfIntersections() {}

  /** @param {Mesh} other @returns {boolean} */
  intersectsMesh(other) {}


  // --- Convex ---------------------------------------------------------------

  /** Convex hull of the mesh. @returns {Mesh} */
  convexHull() {}

  /**
   * Approximate convex decomposition (V-HACD). Each returned mesh is convex
   * and suitable for physics collision.
   * @param {Object} [params]
   * @param {number} [params.maxHulls=16]
   * @param {number} [params.maxVerticesPerHull=64]
   * @param {number} [params.resolution=100000]
   * @param {number} [params.minVolumePerHull=0.001]
   * @returns {Mesh[]}
   */
  convexDecomposition(params) {}


  // --- UV -------------------------------------------------------------------

  /**
   * Automatic UV unwrapping. Modifies uvs in-place.
   * @returns {{ atlasWidth: number, atlasHeight: number, chartCount: number, success: boolean }}
   */
  unwrapUVs() {}

  /**
   * Project UVs using a simple projection mode.
   * @param {string} type - "box"|"planarXY"|"planarXZ"|"planarYZ"|"cylindrical"|"spherical"
   * @param {number} [scale=1]
   * @returns {Mesh} this
   */
  projectUVs(type, scale) {}

  /**
   * Per-triangle UV distortion (one entry per triangle).
   * @returns {Array<{ stretch, areaDistortion, angleDistortion }>}
   */
  computeUVDistortion() {}

  /**
   * Aggregate UV quality metrics for the whole mesh.
   * @returns {{ avgStretch, maxStretch, avgAreaDistortion, maxAreaDistortion,
   *             avgAngleDistortion, maxAngleDistortion, uvSpaceUsage, triangleCount }}
   */
  measureUVQuality() {}


  // --- Vertex-space baking (mutating, return this) --------------------------

  /**
   * Bake ambient occlusion into vertex colors.
   * @param {number} [numRays=64]
   * @param {number} [maxDistance=0] - 0 = auto from bbox
   * @returns {Mesh} this
   */
  bakeAmbientOcclusion(numRays, maxDistance) {}

  /** Bake mean curvature into vertex colors. @returns {Mesh} this */
  bakeCurvature(scale) {}

  /** Bake mesh thickness into vertex colors (SSS approximation). @returns {Mesh} this */
  bakeThickness(numRays, maxDistance) {}


  // --- Texture-space baking -------------------------------------------------
  //
  // Each texture bake returns:
  //   { width, height, channels, pixels: Float32Array }
  // Pixels are row-major, bottom-to-top (OpenGL convention).
  // Requires the mesh to have UVs in [0,1].

  /** @returns {TextureBuffer} 1-channel grayscale AO */
  bakeAOToTexture(texWidth, texHeight, numRays, maxDistance) {}

  /** @returns {TextureBuffer} 1-channel curvature */
  bakeCurvatureToTexture(texWidth, texHeight, scale) {}

  /** @returns {TextureBuffer} 1-channel thickness */
  bakeThicknessToTexture(texWidth, texHeight, numRays, maxDistance) {}

  /** @returns {TextureBuffer} 4-channel RGBA normals (xyz→[0,1], alpha=1) */
  bakeNormalsToTexture(texWidth, texHeight) {}

  /** @returns {TextureBuffer} 4-channel RGBA world position (alpha=1) */
  bakePositionToTexture(texWidth, texHeight) {}

  /**
   * Bake tangent-space normals from a high-poly reference mesh onto this
   * low-poly mesh's UV layout.
   * @param {Mesh} reference - high-poly source
   * @param {number} [searchDistance=0] - 0 = auto
   * @returns {TextureBuffer} 4-channel RGBA tangent-space normals
   */
  bakeNormalsFromReference(reference, texWidth, texHeight, searchDistance) {}

  /**
   * Bake AO against a reference mesh onto this mesh's UV layout.
   * @returns {TextureBuffer} 1-channel AO
   */
  bakeAOFromReference(reference, texWidth, texHeight, numRays, maxDistance) {}


  // --- File I/O (save) ------------------------------------------------------

  /** @param {string} path @returns {boolean} */ saveGLTF(path) {}
  /** @param {string} path @returns {boolean} */ saveOBJ(path) {}
  /** @param {string} path @returns {boolean} */ savePLY(path) {}
  /** @param {string} path @returns {boolean} */ saveSTL(path) {}
}


// -----------------------------------------------------------------------------
// MeshBVH
// -----------------------------------------------------------------------------
// Bounding-volume hierarchy built once per static Mesh, then reused to
// accelerate repeated ray queries. The BVH references triangle indices of the
// source mesh and must be rebuilt if the mesh geometry changes.

class MeshBVH {

  /**
   * Build a BVH for the given mesh.
   * @param {Mesh} mesh
   * @param {number} [leafSize=8] - max triangles per leaf node
   */
  constructor(mesh, leafSize) {}

  /** @returns {boolean} true if the BVH has no triangles */
  get empty() {}

  /** @returns {number} total node count */
  get nodeCount() {}

  /** @returns {number} indexed triangle count */
  get triangleCount() {}

  /**
   * Root bounding box.
   * @returns {{ min: number[], max: number[], centerX, centerY, centerZ, extentX, extentY, extentZ }}
   */
  bounds() {}

  /**
   * Cast a ray against the source mesh using this BVH.
   * @param {Mesh} mesh - must be the same mesh the BVH was built against
   * @param {number[]} origin - [x, y, z]
   * @param {number[]} direction - [x, y, z] (need not be unit length)
   * @param {number} [maxDistance=0] - 0 = unlimited
   * @returns {?RayHit}
   */
  raycast(mesh, origin, direction, maxDistance) {}

  /** Early-out hit test. @returns {boolean} */
  raycastTest(mesh, origin, direction, maxDistance) {}
}


// -----------------------------------------------------------------------------
// ProgressiveMesh
// -----------------------------------------------------------------------------
// A full-resolution mesh plus an ordered edge-collapse sequence. Extract a
// specific triangle count or LOD ratio on demand; serialize to a binary blob
// for streaming.

class ProgressiveMesh {

  /**
   * Build a progressive mesh via QEM-driven edge collapses.
   * @param {Mesh} mesh
   */
  constructor(mesh) {}

  /** Triangle count at full resolution. @returns {number} */
  get maxTriangles() {}

  /** Minimum reachable triangle count. @returns {number} */
  get minTriangles() {}

  /** Number of collapse records stored. @returns {number} */
  get collapseCount() {}

  /**
   * Extract a mesh at a specific triangle count.
   * @param {number} targetTriangles - clamped to [minTriangles, maxTriangles]
   * @returns {Mesh}
   */
  atTriangleCount(targetTriangles) {}

  /**
   * Extract a mesh at a given LOD ratio.
   * @param {number} ratio - 0.0 = minimum, 1.0 = full resolution
   * @returns {Mesh}
   */
  atRatio(ratio) {}

  /**
   * Serialize to a compact binary blob for streaming.
   * @returns {Uint8Array}
   */
  serialize() {}

  /**
   * Deserialize from a binary blob.
   * @param {Uint8Array} bytes
   * @returns {ProgressiveMesh}
   */
  static deserialize(bytes) {}
}


// =============================================================================
// Rigging / animation
// =============================================================================
//
// Conventions
//   Quaternions are xyzw.
//   Matrices are column-major 4x4 (16 floats), matching glTF.
//   Pose data layout (stride 10 per bone): T (3), R (4, xyzw), S (3).
//   Bone indices in skin/animation refer to Skeleton::bones order.
//
// A typical rigging flow:
//   const mesh = Mesh.loadGLTF(path).meshes[0];     // or autoRig from scratch
//   const spec = Rig.spec('humanoid');
//   const lm   = Rig.detectHumanoid(mesh);
//   const r    = Rig.autoRig(mesh, spec, lm);
//   const cycle = Rig.generateLocomotionCycle(r.skeleton, spec, { strideLength: 0.4 });
//
//   // Per frame:
//   const pose    = cycle.evaluate(r.skeleton, t);
//   const matrices = pose.computeSkinningMatrices(r.skeleton);
//   mesh.applySkinning(r.skin, matrices);


// -----------------------------------------------------------------------------
// SkinData
// -----------------------------------------------------------------------------
class SkinData {
  /**
   * @param {Object} [opts]
   * @param {Float32Array} [opts.boneWeights]         - 4 weights per vertex (stride 4)
   * @param {Uint32Array}  [opts.boneIndices]         - 4 indices per vertex (stride 4)
   * @param {Float32Array} [opts.inverseBindMatrices] - column-major mat4 per bone (stride 16)
   * @param {number}       [opts.boneCount]           - explicit bone count (else derived from IBM)
   */
  constructor(opts) {}

  /** @type {Float32Array} */ boneWeights;
  /** @type {Uint32Array}  */ boneIndices;
  /** @type {Float32Array} */ inverseBindMatrices;
  /** @type {number}       */ boneCount;
  /** @type {number}       */ vertexCount;

  /** Deep copy. */
  clone() {}

  /** Normalize per-vertex weights to sum=1; returns this. */
  normalize() {}

  /**
   * Validate skinning quality.
   * @param {Mesh}     mesh
   * @param {SkinData} skin
   * @param {Object}   [options]
   * @param {number}   [options.influences=4]
   * @param {number}   [options.sumTolerance=1e-3]
   * @returns {{ vertexCount: number, orphanCount: number, badSumCount: number,
   *            nanCount: number, maxSumDeviation: number,
   *            maxInfluencesObserved: number, clean: boolean }}
   */
  static validate(mesh, skin, options) {}

  /**
   * Project skin weights from `sourceMesh`/`sourceSkin` onto `targetMesh` by
   * closest-point interpolation. Same Skeleton applies to the result.
   * @param {Mesh} targetMesh
   * @param {Mesh} sourceMesh
   * @param {SkinData} sourceSkin
   * @param {number} [maxDistance=0]   - 0 = unlimited
   * @returns {SkinData}
   */
  static transfer(targetMesh, sourceMesh, sourceSkin, maxDistance) {}
}


// -----------------------------------------------------------------------------
// Skeleton
// -----------------------------------------------------------------------------
class Skeleton {
  /**
   * @param {Object} [opts]
   * @param {Array}  [opts.bones]   - [{name, parent, localT, localR, localS, inverseBind}]
   * @param {Array}  [opts.sockets] - [{name, bone, offset}]
   */
  constructor(opts) {}

  /** Construct from a flat bone array. */
  static fromBones(bones) {}

  /** @type {Array} */ bones;
  /** @type {Array} */ sockets;
  /** @type {number} */ boneCount;
  /** @type {number} */ socketCount;

  findBone(name) {}     // -> int or -1
  findSocket(name) {}   // -> int or -1

  /** Append a socket; returns its new index. */
  addSocket(socket) {}

  /** Identity / bind-pose. */
  bindPose() {}         // -> Pose

  /** Append the standard Rigify-style sockets if matching bones exist. */
  addRigifySockets() {} // -> int (count added)

  /** Find a bone whose name ends with `suffix`. -1 if none. */
  findBoneBySuffix(suffix) {}
}


// -----------------------------------------------------------------------------
// Pose
// -----------------------------------------------------------------------------
class Pose {
  /**
   * @param {number|Float32Array|{data:Float32Array}} [opts]
   *        number          → zero-init pose with identity TRS for that many bones
   *        Float32Array    → adopt as raw data
   *        {data}          → adopt from object
   */
  constructor(opts) {}

  /** @type {Float32Array} - stride 10/bone: T(3), R(4 xyzw), S(3) */ data;
  /** @type {number} */ boneCount;

  clone() {}

  /** Float32Array of 16*boneCount column-major mat4 world transforms. */
  computeWorldMatrices(skeleton) {}

  /** Float32Array of 16*boneCount column-major mat4 world * inverseBind. */
  computeSkinningMatrices(skeleton) {}

  /** mat4 (Float32Array(16)) for a named socket, or null if unknown. */
  socketWorld(skeleton, socketName) {}

  /**
   * Blend `b` into `a` in-place (lerp T/S, slerp R). Returns `a`.
   * Optional `mask` is a Uint8Array of length boneCount; 1 enables blend.
   */
  static blend(a, b, weight, mask) {}
}


// -----------------------------------------------------------------------------
// Animation
// -----------------------------------------------------------------------------
class Animation {
  /**
   * @param {Object} opts
   * @param {string} [opts.name]
   * @param {number} [opts.duration]   - seconds; auto-derived if omitted
   * @param {Array}  [opts.channels]   - [{boneIndex, path, interp, times, values}]
   *                                     path:   'translation' | 'rotation' | 'scale'
   *                                     interp: 'linear' | 'step' | 'cubicSpline'
   */
  constructor(opts) {}

  /** @type {string} */ name;
  /** @type {number} */ duration;
  /** @type {Array}  */ channels;
  /** @type {number} */ channelCount;

  /** Evaluate at time t (seconds). loop defaults to true. */
  evaluate(skeleton, t, options) {}      // -> Pose
  evaluateInto(skeleton, t, pose, options) {} // -> pose (same instance)

  /** Remap a clip from src to dst skeleton by bone name. */
  static retarget(anim, srcSkeleton, dstSkeleton) {}
}


// -----------------------------------------------------------------------------
// IK — solvers (mutate the supplied Pose in place; return bool)
// -----------------------------------------------------------------------------

/** Two-bone analytic IK (root, mid, end). target / pole are [x,y,z] world. */
function IK_twoBone(skel, pose, root, mid, end, target, pole) {}

/** FABRIK solver for arbitrary chain (array of bone indices). */
function IK_FABRIK(skel, pose, chain, target, options) {}
// options: { iterations: 10, tolerance: 1e-3 }

/** Look-at orientation on a single bone. */
function IK_lookAt(skel, pose, bone, target, options) {}
// options: { localForward: [0,0,1], localUp: [0,1,0] }


// -----------------------------------------------------------------------------
// Rig — high-level pipeline (global namespace object)
// -----------------------------------------------------------------------------

/** Built-in spec by name: 'humanoid' | 'quadruped' | 'hexapod' | 'octopod'. */
function Rig_spec(name) {}             // -> RigSpec (empty if unknown)
function Rig_specFromJSON(jsonText) {} // -> RigSpec
function Rig_specFromFile(path) {}     // -> RigSpec

/** Geometric landmark detection (returns { name: [x,y,z], ... }). */
function Rig_detectHumanoid(mesh) {}
function Rig_detectQuadruped(mesh) {}

/** Names of spec landmarks not present in the supplied dict. */
function Rig_missingLandmarks(spec, landmarks) {}  // -> string[]

/** Build a Skeleton bound to a mesh via spec + landmarks. */
function Rig_fitSkeleton(spec, landmarks, mesh) {} // -> Skeleton

/**
 * One-call autoRig:
 * @returns {{ skeleton: Skeleton, skin: SkinData,
 *             missingLandmarks: string[], warnings: string[],
 *             methodUsed: string }}
 */
function Rig_autoRig(mesh, spec, landmarks, options) {}
// options: { method: 'auto'|'voxelBind'|'boneHeat'|'bbw',
//            smoothIterations: 2, smoothAlpha: 0.5, minWeight: 1e-3 }

/** Synthesized walk/run cycle from skeleton + spec. */
function Rig_generateLocomotionCycle(skeleton, spec, params) {} // -> Animation
// params: { strideLength, cycleDuration, footLiftHeight, keyframesPerCycle,
//           bodyBobAmplitude, gait: { name, phases:[float], dutyFactor } }


// -----------------------------------------------------------------------------
// VoxelChunk
// -----------------------------------------------------------------------------
class VoxelChunk {
  /**
   * @param {number} sizeX
   * @param {number} sizeY
   * @param {number} sizeZ
   * @param {number} [cellSize=1]
   */
  constructor(sizeX, sizeY, sizeZ, cellSize) {}

  /** @type {number}  */ sizeX;
  /** @type {number}  */ sizeY;
  /** @type {number}  */ sizeZ;
  /** @type {number}  */ cellSize;
  /** @type {boolean} */ isDirty;

  getVoxel(x, y, z) {}                  // -> int (0 if OOB)
  setVoxel(x, y, z, material) {}        // chainable
  fill(value) {}                        // chainable
  markDirty() {}
  clearDirty() {}
  data() {}                             // -> Uint8Array (copy)
  setData(uint8Array) {}                // chainable; bulk copy + markDirty
  buildMesh(palette, paletteCount) {}   // -> Mesh; palette = Float32Array (4 per material)
}


// -----------------------------------------------------------------------------
// glTF rigged extensions
// -----------------------------------------------------------------------------
// Mesh.loadGLTF(path) returns:
//   {
//     meshes:            Mesh[],
//     skins:             SkinData[],
//     skeletons:         Skeleton[],
//     animations:        Animation[],
//     meshSkeleton:      number[],   // index into skeletons (-1 if unskinned)
//     animationSkeleton: number[],   // index into skeletons
//   }
//
// mesh.saveGLTF(path, opts?) — opts: { skin?, skeleton?, animations? }
//   Without opts, saves an unskinned mesh (back-compat).
//   With opts.skeleton, animations may be supplied.
