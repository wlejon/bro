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
//   PolyMesh         — half-edge mesh over N-gon faces with face groups.
//                      Edit topology (extrude, translate, merge by group)
//                      independent of triangulation; tessellate() on demand
//                      to produce a render-ready triangle mesh.
//   LSystem          — string-rule stochastic L-system rewriter (axiom +
//                      production rules → derived module sequence).
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
   * Cone along the Y axis. Base disc sits at Y=0 with radius `radius`;
   * apex sits at Y=`height`. With `capBase=true` a fan cap is added at
   * Y=0 facing -Y; otherwise the bottom is open.
   * @param {number} [radius=0.5]
   * @param {number} [height=1]
   * @param {number} [slices=16]
   * @param {number} [stacks=4]
   * @param {boolean} [capBase=false]
   * @returns {Mesh}
   */
  static cone(radius, height, slices, stacks, capBase) {}

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
   * Noise-displaced sphere with non-uniform scale and an optional center
   * baked into the geometry. Equivalent to
   *   Mesh.rock(radius, seed, nsub).scale(...).translate(...)
   * but skips the intermediate copies and recomputes normals only when the
   * scale is non-uniform.
   *
   * @param {Object} [opts]
   * @param {number} [opts.radius=0.5]
   * @param {number} [opts.seed=42]
   * @param {number} [opts.nsub=2]
   * @param {number|number[]} [opts.scale=1]   number = uniform; [x,y,z] = per-axis
   * @param {number[]} [opts.center=[0,0,0]]
   * @returns {Mesh}
   *
   * @example
   *   const canopyBlob = Mesh.blob({
   *     radius: 1.2, seed: 17, nsub: 2,
   *     scale: [1.0, 0.7, 1.0],     // squashed sphere
   *     center: [0, 4, 0],
   *   });
   */
  static blob(opts) {}

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

  /** Load an FBX file. Read-only — there is no saveFBX. @returns {Mesh[]} */
  static loadFBX(path) {}

  /** Load a PLY file. @returns {Mesh} */
  static loadPLY(path) {}

  /** Load a binary STL file. @returns {Mesh} */
  static loadSTL(path) {}

  /**
   * Load a MagicaVoxel VOX file. Read-only — there is no saveVOX.
   * Pair with `Mesh.greedyMesh(voxels, sizeX, sizeY, sizeZ, 1, palette)` or
   * `new VoxelChunk(...)` + `setData(voxels)` to build geometry.
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


// -----------------------------------------------------------------------------
// PolyMesh
// -----------------------------------------------------------------------------
// Half-edge mesh over N-gon (not necessarily triangular) faces, with per-face
// integer "group" tags. Use this when you want to edit topology — extrude a
// face, translate a face and have its surrounding ring follow, merge coplanar
// faces by group — that survives arbitrary triangulation choices. The render
// mesh is produced on demand via tessellate().
//
// Typical flow (face-group editing in scene-editor):
//   const pm = PolyMesh.fromMeshData(positions, indices, triToGroup);
//   const ring = pm.extrudeFace(faceIdx, [0, 0.5, 0]);    // pull a face up
//   pm.translateFaceWithRing(faceIdx, [0.1, 0, 0]);       // nudge with neighbors
//   pm.mergeFacesByGroup();                               // collapse coplanar
//   const tess = pm.tessellate();
//   // tess: { positions, normals, indices, triToFace, triToGroup }
//
// Conventions
//   Face indices are stable until compact() is called.
//   group = -1 typically means "unassigned" / inherits from neighbor.
//   Vertex/face/half-edge accessors clamp to the mesh's current size; out-of
//   -range queries return -1 / [] / null without throwing.

class PolyMesh {

  /** Construct an empty PolyMesh. Use the static factories for populated meshes. */
  constructor() {}

  // --- Static factories -----------------------------------------------------

  /**
   * Build from a triangle mesh, optionally with a per-triangle group tag.
   * Coplanar adjacent triangles sharing a group can be merged into N-gons
   * later via mergeFacesByGroup().
   * @param {Float32Array} positions - xyz, stride 3
   * @param {Uint32Array}  indices   - triangle indices
   * @param {Uint32Array|Int32Array} [triToGroup] - one group id per triangle
   * @returns {PolyMesh}
   */
  static fromMeshData(positions, indices, triToGroup) {}

  /**
   * Build a single N-gon face from an ordered ring of vertex positions.
   * @param {Float32Array} positions - xyz, stride 3, in winding order
   * @param {number[]} normal        - [nx, ny, nz] face normal (orientation hint)
   * @param {number} [group=0]       - face group tag
   * @returns {PolyMesh}
   */
  static fromPolygon(positions, normal, group) {}


  // --- Counts ---------------------------------------------------------------

  /** @returns {number} */ get vertexCount() {}
  /** @returns {number} */ get halfEdgeCount() {}
  /** @returns {number} */ get faceCount() {}


  // --- Inspection -----------------------------------------------------------

  /** @param {number} faceIdx @returns {number} ring length (3+) */
  faceVertexCount(faceIdx) {}

  /** @param {number} faceIdx @returns {number[]} ordered vertex indices */
  faceVertices(faceIdx) {}

  /** @param {number} vertexIdx @returns {?number[]} [x, y, z] */
  getVertex(vertexIdx) {}

  /** Newell's-method face normal. @returns {?number[]} [nx, ny, nz] */
  computeFaceNormal(faceIdx) {}

  /** @returns {number} group id, or -1 if face out of range */
  faceGroup(faceIdx) {}

  /** Set a face's group tag (mutates in place). */
  setFaceGroup(faceIdx, group) {}

  /** @param {number} groupId @returns {number[]} face indices in that group */
  facesInGroup(groupId) {}


  // --- Tessellation ---------------------------------------------------------

  /**
   * Triangulate every face into a render-ready buffer. Smooth normals and
   * back-pointers from each triangle to its source face / group are emitted.
   * @returns {{ positions: Float32Array, normals: Float32Array,
   *             indices: Uint32Array,
   *             triToFace: Uint32Array, triToGroup: Uint32Array }}
   */
  tessellate() {}


  // --- Validation -----------------------------------------------------------

  /**
   * Check half-edge invariants (twin pairing, face cycles, etc.).
   * @returns {{ valid: boolean, isClosed: boolean,
   *             boundaryHalfEdges: number, errors: string[] }}
   */
  validate() {}


  // --- Mutation -------------------------------------------------------------

  /** Append a new vertex; returns its index. */
  addVertex(x, y, z) {}

  /**
   * Add a face from an ordered ring of existing vertex indices.
   * @param {number[]} verts
   * @param {number} [group=-1]
   * @returns {number} new face index, or -1 on failure
   */
  addFace(verts, group) {}

  /** Translate a single vertex by [dx, dy, dz]. */
  translateVertex(vertexIdx, offset) {}

  /** Translate every vertex of a face by [dx, dy, dz]. */
  translateFace(faceIdx, offset) {}

  /**
   * Translate a face and the one-ring of vertices around it. Useful for
   * "soft-pull" handle dragging in editors.
   */
  translateFaceWithRing(faceIdx, offset) {}

  /**
   * Push a face out along an offset, optionally bridging the gap with side
   * faces (and capping the back). Vertices on the moving rim are duplicated
   * so neighbors are not disturbed.
   * @param {number} faceIdx
   * @param {number[]} offset           - [dx, dy, dz]
   * @param {boolean} [withBack=true]   - cap the back so the result stays closed
   * @param {number}  [bridgeGroup=-1]  - group id for newly created side faces
   * @param {number}  [backGroup=-1]    - group id for the back cap
   * @returns {{ dupVerts: Uint32Array, bridgeFaces: Uint32Array,
   *             bridgeAdjGroup: Uint32Array, backFace: number }}
   */
  extrudeFace(faceIdx, offset, withBack, bridgeGroup, backGroup) {}

  /** Re-pair half-edge twins after structural edits (no-op if already paired). */
  rematchTwins() {}

  /**
   * Merge adjacent coplanar faces that share a group id into single N-gons.
   * Run after extrudeFace + setFaceGroup edits to clean up topology.
   */
  mergeFacesByGroup() {}

  /** Drop dead vertices/edges/faces and renumber indices. Call sparingly. */
  compact() {}


  // --- Boundaries -----------------------------------------------------------

  /**
   * Extract the boundary loop(s) of a face group as ordered vertex rings.
   * @param {number} groupId
   * @returns {number[][]} one inner array per loop (multiple if the group is
   *                       not simply connected)
   */
  findGroupBoundary(groupId) {}
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

// -----------------------------------------------------------------------------
// Plant primitives — leafCard, flower, bezierSweep
// -----------------------------------------------------------------------------
//
// Procedural plant shapes for foliage and bloom rendering. UVs on leafCard /
// flower sample a 4x4 atlas grid (procedural generation up to the consumer);
// cell mapping by shape:
//
//   'oval'    -> (col 0, row 0)
//   'pointed' -> (col 1, row 0)
//   'lobed'   -> (col 2, row 0)
//   'needle'  -> (col 3, row 0)
//   'frond'   -> (col 0, row 1)
//   'petal'   -> (col 1, row 1)
//
// Vertex colors (R channel) carry per-vertex windBend [0..1] consumed by
// scene.setWind — base of the leaf is anchored (0), the tip sways (1).

/**
 * Build a low-poly leaf or petal card lying in the XZ plane (Y is the card
 * normal). Uses a (widthSegments+1)x(lengthSegments+1) grid so `bend` and
 * `curl` look smooth; defaults are 4x8.
 *
 * @param {('oval'|'pointed'|'lobed'|'needle'|'frond'|'petal')} shape
 * @param {Object}  [opts]
 * @param {number}  [opts.width=0.4]
 * @param {number}  [opts.length=1.0]
 * @param {number}  [opts.bend=0]            radians of forward arc deflection
 * @param {number}  [opts.curl=0]            radians of roll around length axis
 * @param {boolean} [opts.stemOffset=true]   pivot at base when true
 * @param {number}  [opts.widthSegments=4]
 * @param {number}  [opts.lengthSegments=8]
 * @param {boolean} [opts.fullUV=false]      span UVs over [0,1] instead of an
 *                                           atlas sub-cell; `shape` is ignored
 * @returns {Mesh}
 *
 * @example
 *   const leaf = Mesh.leafCard('oval', { width: 0.3, length: 0.8, bend: 0.4 });
 */
Mesh.leafCard = function(shape, opts) {};

/**
 * Build a radial flower: a small dome center with `petalCount` leaf cards
 * arranged in `layers` rings. layers > 1 stacks rings with progressively
 * smaller radii and `layerTwist` rotation between them.
 *
 * @param {Object} [opts]
 * @param {number} [opts.petalCount=6]
 * @param {string} [opts.petalShape='petal']
 * @param {number} [opts.petalLength=0.5]
 * @param {number} [opts.petalWidth=0.25]
 * @param {number} [opts.petalCurl=0]
 * @param {number} [opts.petalBend=0.6]
 * @param {number} [opts.layers=1]
 * @param {number} [opts.layerTwist=0.4]
 * @param {number} [opts.centerRadius=0.08]
 * @param {number} [opts.centerHeight=0.04]
 * @param {number[]} [opts.centerColor=[1,0.85,0.2]]
 * @returns {Mesh}
 *
 * @example
 *   const rose = Mesh.flower({ petalCount: 8, layers: 3, layerTwist: 0.5 });
 */
Mesh.flower = function(opts) {};

/**
 * Sweep a 2D profile along a cubic-bezier polyline. controlPoints is treated
 * as a sequence of cubic segments where every group of 4 points (P0,P1,P2,P3)
 * defines one segment, and consecutive segments share their endpoint — for two
 * segments pass 7 points (P0,P1,P2,P3=Q0,Q1,Q2,Q3). Length must satisfy
 * (N-1) % 3 == 0 and N >= 4.
 *
 * @param {number[][]} controlPoints  [x,y,z] triples; (N-1) % 3 == 0
 * @param {number[][]} profile        [x,y] pairs, swept perpendicular to path
 * @param {Object} [opts]
 * @param {number} [opts.samples=32]
 * @param {number[]} [opts.profileScale]  resampled to length=samples
 * @param {number[]} [opts.twist]         radians, resampled to length=samples
 * @param {boolean} [opts.capStart=true]
 * @param {boolean} [opts.capEnd=true]
 * @param {boolean} [opts.closeProfile=true]
 * @param {boolean} [opts.miterJoints=true]
 * @returns {Mesh}
 *
 * @example
 *   const stem = Mesh.bezierSweep(
 *     [[0,0,0], [0.05,0.4,0], [-0.1,0.8,0], [0,1.2,0]],
 *     [[0.03,0],[0,0.03],[-0.03,0],[0,-0.03]],
 *     { samples: 32, profileScale: [1.0, 0.5] }
 *   );
 */
Mesh.bezierSweep = function(controlPoints, profile, opts) {};

/**
 * Sweep a circular cross-section of `sides` vertices along `path`, scaled
 * per-ring by `radius`. Convenience wrapper over `sweep` for the common
 * branch / vine / stem case — equivalent to `sweep(circleProfile(sides),
 * path, { profileScale: radius, ... })` but spares the caller the profile
 * setup.
 *
 * @param {number[][]|Float32Array} path     Vec3 polyline (>= 2 points)
 * @param {number|number[]}         radius   constant or per-vertex (length must == path.length)
 * @param {number} [sides=8]                 ring resolution (>= 3)
 * @param {Object} [opts]
 * @param {boolean} [opts.capStart=true]
 * @param {boolean} [opts.capEnd=true]
 * @param {boolean} [opts.miterJoints=true]
 * @returns {Mesh}
 *
 * @example
 *   const stem = Mesh.tube(
 *     [[0,0,0],[0,0.5,0],[0.1,1.0,0]],
 *     [0.04, 0.03, 0.01],
 *     6
 *   );
 */
Mesh.tube = function(path, radius, sides, opts) {};

/**
 * Sweep a 4-vertex diamond profile along `path`, producing a thin tapered
 * blade-like ribbon with optional out-of-plane thickness. Convenience
 * wrapper over `sweep` for grass blades, fern leaflets, succulent leaves —
 * spares the caller the diamond-profile setup.
 *
 * `width` and `thickness` are half-extents along profile-X and profile-Y;
 * `thickness: 0` collapses to a flat ribbon.
 *
 * @param {number[][]|Float32Array} path     Vec3 polyline (>= 2 points)
 * @param {Object} [opts]
 * @param {number}  [opts.width=0.05]
 * @param {number}  [opts.thickness=0]
 * @param {number[]} [opts.profileScale]   size 0/1 = constant; else length == path.length
 * @param {number[]} [opts.twist]          radians per ring; size rules as profileScale
 * @param {boolean} [opts.capStart=false]
 * @param {boolean} [opts.capEnd=true]
 * @param {boolean} [opts.miterJoints=true]
 * @returns {Mesh}
 *
 * @example
 *   const path = Mesh.bladePath({ length: 0.4, bend: 0.15, segments: 8 });
 *   const blade = Mesh.bladeStrip(path, {
 *     width: 0.012, thickness: 0.002,
 *     profileScale: path.map((_, i) => 1 - i / (path.length - 1)),
 *   });
 */
Mesh.bladeStrip = function(path, opts) {};

/**
 * Build a smooth path from `base` to `base + tipDir·length` using a
 * quadratic Bezier. The control point sits at the midpoint, offset by
 * `bend` on a lateral axis (world +X projected perpendicular to tipDir,
 * falling back to +Z) and `lift` on world +Y. Returns segments+1 points
 * consumable by `Mesh.bladeStrip` or `Mesh.sweep`.
 *
 * @param {Object} [opts]
 * @param {number[]} [opts.base=[0,0,0]]
 * @param {number[]} [opts.tipDir=[0,1,0]]   normalized internally
 * @param {number}   [opts.length=1]
 * @param {number}   [opts.bend=0]           lateral midpoint offset
 * @param {number}   [opts.lift=0]           world +Y midpoint offset
 * @param {number}   [opts.segments=8]
 * @returns {number[][]}                     segments+1 [x,y,z] triples
 *
 * @example
 *   const path = Mesh.bladePath({
 *     base: [0, 0, 0], tipDir: [0, 1, 0],
 *     length: 0.4, bend: 0.12, segments: 8,
 *   });
 */
Mesh.bladePath = function(opts) {};

/**
 * Sweep a 2D profile (in the local XY plane) along a 3D polyline `path` using
 * parallel-transport (rotation-minimizing) frames. The lower-level building
 * block that `bezierSweep` wraps — pass an arbitrary path rather than a
 * cubic-bezier control polygon. Returns a triangulated mesh with smooth
 * ring-averaged normals; caps assume the profile is convex (concave caps fan
 * from the centroid and may overlap).
 *
 * @param {number[][]|Float32Array} profile  [x,y] pairs, swept perpendicular to path
 * @param {number[][]|Float32Array} path     [x,y,z] triples; ring per point
 * @param {Object}   [opts]
 * @param {boolean}  [opts.closeProfile=true]
 * @param {boolean}  [opts.capStart=true]
 * @param {boolean}  [opts.capEnd=true]
 * @param {boolean}  [opts.miterJoints=true]   bisector ring at interior path verts
 * @param {number[]} [opts.profileScale]       size 0/1 = constant; else length == path.length
 * @param {number[]} [opts.twist]              radians per ring; size rules as profileScale
 * @returns {Mesh}
 *
 * @example
 *   const tube = Mesh.sweep(
 *     [[0.05,0],[0,0.05],[-0.05,0],[0,-0.05]],
 *     [[0,0,0],[0,0.5,0],[0.2,1.0,0]]
 *   );
 */
Mesh.sweep = function(profile, path, opts) {};


// -----------------------------------------------------------------------------
// Procedural branches — space colonization, pipe-model thickening, sweep mesh
// -----------------------------------------------------------------------------
//
// Pipeline:
//   1. spaceColonize(attractors, seeds, dir, opts) -> BranchSegment[]
//   2. thickenBranches(segments, leafRadius, pipeExp) -> BranchSegment[]
//   3. meshBranches(segments, sides) -> Mesh
//   4. (optional) placeLeavesOnBranches / scatterLeaves to attach foliage
//
// BranchSegment shape:
//   { parent: number,    // index of parent segment, or -1 for root
//     from:   [x,y,z],
//     to:     [x,y,z],
//     radius: number,    // 0 until thickenBranches runs
//     depth:  number }   // graph depth from root

/**
 * Runions-style space colonization. Grows branch segments toward a cloud of
 * `attractors` from one or more `seedPoints`. Roots (one per seed) have
 * `parent === -1` and `from === to === seedPoint`.
 *
 * @param {number[][]|Float32Array} attractors    [x,y,z] cloud of growth targets
 * @param {number[][]|Float32Array} seedPoints    [x,y,z] root positions
 * @param {number[]}                initialDirection  [x,y,z] starting growth direction
 * @param {Object} [opts]
 * @param {number} [opts.attractionRadius=5]
 * @param {number} [opts.killRadius=0.5]
 * @param {number} [opts.segmentLength=0.3]
 * @param {number} [opts.maxIterations=200]
 * @param {number[]} [opts.tropism=[0,0,0]]    biased direction added every step
 * @param {number} [opts.tropismWeight=0]
 * @param {CapsuleField} [opts.obstacles]      growth steps that would land
 *   inside an obstacle are rejected (or steered around — see below). Pass a
 *   field built via `Mesh.capsuleFieldFromSegments(prevSegs)` to grow new
 *   canes that avoid earlier ones; combine iteratively for self-avoiding
 *   bushes.
 * @param {number} [opts.obstacleClearance=0]  inflate every obstacle by this
 *   much during the test (extra slack between growth and surfaces).
 * @param {number} [opts.obstacleSteer=0]      radians; 0 = hard reject the
 *   blocked step. >0 = rotate the growth direction up to that many radians
 *   toward the obstacle's surface tangent before re-testing; if still
 *   colliding, project the candidate outward along the local normal so the
 *   tree slides past instead of stalling.
 * @returns {Array<BranchSegment>}
 */
Mesh.spaceColonize = function(attractors, seedPoints, initialDirection, opts) {};

/**
 * Pipe-model branch radii. Leaves get `leafRadius`; each parent's radius is
 *   r_parent = (sum of r_child^pipeExp)^(1/pipeExp).
 * Returns the same segment array with `radius` populated.
 *
 * @param {Array<BranchSegment>} segments
 * @param {number} [leafRadius=0.02]
 * @param {number} [pipeExp=2.5]
 * @returns {Array<BranchSegment>}
 */
Mesh.thickenBranches = function(segments, leafRadius, pipeExp) {};

/**
 * Mesh a branch tree as one merged sweep. Each maximal chain of single-child
 * segments shares parallel-transport orientation across rings, and forks blend
 * via the parent's continuing radius (no flat disc capping at branch points).
 *
 * @param {Array<BranchSegment>} segments
 * @param {number} [sides=8]   cross-section ring resolution (>= 3)
 * @returns {Mesh}
 *
 * @example
 *   const segs = Mesh.spaceColonize(cloud, [[0,0,0]], [0,1,0],
 *                                   { tropism: [0,1,0], tropismWeight: 0.3 });
 *   Mesh.thickenBranches(segs, 0.015, 2.5);
 *   const trunk = Mesh.meshBranches(segs, 8);
 */
Mesh.meshBranches = function(segments, sides) {};


// -----------------------------------------------------------------------------
// CapsuleField — collision-aware placement substrate
// -----------------------------------------------------------------------------
//
// Capsule + sphere occupancy lookup, built once per scatter / colonize pass
// and queried during placement to avoid interpenetration. Backed by a spatial
// hash; queries are local (O(k) in the obstacles near the query, not the
// total count).
//
// Tag-based exclusion: every query takes an optional integer `excludeTag`.
// When non-negative, capsules / spheres whose `tag` equals it are skipped.
// This lets a leaf scattered along segment N ignore segment N's own capsule.
// `Mesh.capsuleFieldFromSegments` assigns `tag = segment-index` automatically.
//
// Capsule shape:  { a: [x,y,z], b: [x,y,z], radius: number, tag?: number }
// Sphere shape:   { center: [x,y,z], radius: number, tag?: number }

/**
 * Build a CapsuleField from explicit lists.
 *
 * @param {Capsule[]} capsules
 * @param {Sphere[]}  [spheres]
 * @param {number}    [cellSize=0]   0 = auto-pick from input radii / lengths
 * @returns {CapsuleField}
 */
Mesh.capsuleField = function(capsules, spheres, cellSize) {};

/**
 * Build a CapsuleField from a BranchSegment list. Each segment with
 * `radius > 0` becomes a capsule tagged with its segment index. Synthetic
 * roots (from === to) are skipped.
 *
 * @param {Array<BranchSegment>} segments
 * @param {number}    [radiusScale=1]   multiply every capsule radius by this
 *                                      (>1 to inflate clearance, <1 to thin)
 * @param {Sphere[]}  [extraSpheres]    appended to the field
 * @returns {CapsuleField}
 *
 * @example
 *   const skeleton = buildBushSkeleton();
 *   const field    = Mesh.capsuleFieldFromSegments(skeleton.segs, 1.2);
 *   const foliage  = Mesh.scatterLeaves(skeleton.segs, leaf,
 *                                       { perUnitLength: 24, avoid: field });
 */
Mesh.capsuleFieldFromSegments = function(segments, radiusScale, extraSpheres) {};

/**
 * Greedy anchor picker for blooms / fruits / clustered foliage. Visits
 * candidates in a seeded shuffled order and accepts each if it (a) is at
 * least `minSpacing` from every previously-accepted anchor, (b) clears the
 * obstacle field by `minObstacleDistance`, and (c) lies outside every
 * `keepOut` sphere.
 *
 * @param {number[][]|Float32Array} candidates
 * @param {Object} [opts]
 * @param {number} [opts.minSpacing=0]
 * @param {number} [opts.minObstacleDistance=0]
 * @param {number} [opts.maxCount=0]            0 = unlimited
 * @param {CapsuleField} [opts.avoid]
 * @param {Sphere[]}     [opts.keepOut]
 * @param {number} [opts.seed=0]
 * @returns {Int32Array} indices into `candidates` in acceptance order
 *
 * @example
 *   const tipPoints = terminals.map(t => t.p);
 *   const idx = Mesh.packAnchors(tipPoints, {
 *     minSpacing: 0.18,             // blooms can't crowd each other
 *     avoid: skeletonField,         // and stay clear of stems
 *     maxCount: 8,
 *     seed: 42,
 *   });
 *   const bloomCenters = Array.from(idx).map(i => tipPoints[i]);
 */
Mesh.packAnchors = function(candidates, opts) {};

/**
 * @class CapsuleField
 *
 * Static occupancy lookup over capsules + spheres. Constructed via
 * `Mesh.capsuleField(...)` or `Mesh.capsuleFieldFromSegments(...)`. Once
 * built, the field is immutable; rebuild when the underlying geometry
 * changes.
 */
class CapsuleField {
  /**
   * Construct directly from arrays (same as Mesh.capsuleField).
   * @param {Capsule[]} [capsules]
   * @param {Sphere[]}  [spheres]
   * @param {number}    [cellSize=0]
   */
  constructor(capsules, spheres, cellSize) {}

  /** @type {boolean} */ get empty() {}
  /** @type {number}  */ get capsuleCount() {}
  /** @type {number}  */ get sphereCount() {}
  /** @type {number}  */ get cellSize() {}

  /**
   * Signed distance to the nearest obstacle surface. Negative inside,
   * positive outside. +Infinity when the field is empty.
   * @param {number[]} point
   * @param {number}   [excludeTag=-1]
   * @returns {number}
   */
  distance(point, excludeTag) {}

  /**
   * Closest surface point + outward normal + signed distance + tag.
   * @param {number[]} point
   * @param {number}   [excludeTag=-1]
   * @returns {{ point: number[], normal: number[], distance: number, tag: number }}
   */
  nearest(point, excludeTag) {}

  /**
   * True iff a sphere centered at `center` with `radius` overlaps any
   * obstacle (i.e. signed distance < radius).
   * @param {number[]} center
   * @param {number}   radius
   * @param {number}   [excludeTag=-1]
   * @returns {boolean}
   */
  intersectsSphere(center, radius, excludeTag) {}
}


// -----------------------------------------------------------------------------
// Leaf scattering — attach leafCards / flowers to a branch tree
// -----------------------------------------------------------------------------
//
// `placeLeavesOnBranches` returns instance transforms for GPU instancing.
// `scatterLeaves` stamps a leaf mesh per placement and merges into one Mesh.
// Both share the same LeafPlacementOptions:
//
//   maxRadius=0.05      skip branches thicker than this (keeps leaves off trunk)
//   minDepth=1          skip segments with depth below this
//   terminalOnly=false  only on chain tips
//   perUnitLength=20    average leaves per unit of segment length
//   densityFalloff=0    >0 biases samples toward the tip
//   upBias=0.5          0 = radial-out, 1 = world-up forward (phototropism)
//   tiltJitter=0.3      radians ± random pitch around branch tangent
//   rollJitter=0.2      radians ± random roll around leaf forward
//   baseScale=1
//   scaleJitter=0.2     ± fraction of baseScale per leaf
//   scaleByRadius=0     1 = scale linearly with (radius / maxRadius)
//   dedupRadius=0       minimum distance between leaf origins
//   seed=0              deterministic when nonzero
//
//   avoid=null          CapsuleField; candidates whose origin lies within
//                       obstacleClearance of any non-self capsule/sphere are
//                       rejected. The candidate's own segment is excluded via
//                       its segment index — build the field via
//                       `Mesh.capsuleFieldFromSegments(segs)` for this to
//                       work automatically.
//   obstacleClearance=0 extra clearance applied to every avoid test.
//   obstaclePushout=0   if >0, rejected candidates are pushed outward along
//                       the nearest surface normal once and re-tested before
//                       being dropped. Recovers some leaves that would have
//                       been lost to grazing collisions.
//   keepOut=[]          array of {center:[x,y,z], radius} spheres tested in
//                       addition to `avoid` (no exclusion). Use to reserve
//                       volume around bloom anchors so foliage doesn't pack
//                       into the bloom.
//
// Per-leaf local frame matches `leafCard`: +Z = tip, +Y = card normal, +X = side.

/**
 * Compute leaf-instance transforms along branch segments. Useful for GPU
 * instancing (feed `transforms` to an instanced draw).
 *
 * @param {Array<BranchSegment>} segments
 * @param {Object} [opts]   see LeafPlacementOptions table above
 * @returns {{ count: number,
 *             transforms: Float32Array,    // stride 16, column-major mat4 = T·R·S
 *             branchRadius: Float32Array,  // 1 per leaf
 *             branchDepth:  Int32Array }}  // 1 per leaf
 */
Mesh.placeLeavesOnBranches = function(segments, opts) {};

/**
 * Stamp `leaf` (in its local space) at every placement and return one merged
 * mesh. Positions are transformed by the full 4x4; normals by the rotation
 * part (uniform scale, so no inverse-transpose needed).
 *
 * @param {Array<BranchSegment>} segments
 * @param {Mesh}   leaf
 * @param {Object} [opts]   see LeafPlacementOptions table above
 * @returns {Mesh}
 *
 * @example
 *   const leaf = Mesh.leafCard('oval', { width: 0.08, length: 0.18 });
 *   const foliage = Mesh.scatterLeaves(segs, leaf,
 *                                      { maxRadius: 0.03, perUnitLength: 30 });
 */
Mesh.scatterLeaves = function(segments, leaf, opts) {};

/**
 * High-level tree archetype: samples attractors uniformly inside a sphere
 * around `canopyCenter`, runs space colonization from `base` toward them,
 * thickens via the pipe model, and meshes the resulting tree as one merged
 * tube. Foliage is the caller's responsibility — pass the returned
 * `segments` into `Mesh.scatterLeaves` (or `Mesh.placeLeavesOnBranches`)
 * with your own leaf mesh. Deterministic for a given `seed`.
 *
 * @param {Object} [opts]
 * @param {number[]} [opts.base=[0,0,0]]
 * @param {number[]} [opts.canopyCenter=[0,4,0]]
 * @param {number}   [opts.canopyRadius=3]
 * @param {number}   [opts.attractorCount=200]
 * @param {number}   [opts.sides=8]                 tube ring resolution
 * @param {number}   [opts.leafRadius=0.05]         tip radius for thickenBranches
 * @param {number}   [opts.pipeExp=2.5]
 * @param {number}   [opts.seed=1]
 * @param {Object}   [opts.colonize]                forwarded to spaceColonize
 *   { attractionRadius, killRadius, segmentLength, maxIterations,
 *     tropism: [x,y,z], tropismWeight,
 *     obstacles: CapsuleField, obstacleClearance, obstacleSteer }
 * @returns {{ segments: BranchSegment[], branches: Mesh }}
 *
 * @example
 *   const { segments, branches } = Mesh.tree({
 *     canopyCenter: [0, 5, 0], canopyRadius: 2.5,
 *     attractorCount: 250, sides: 6,
 *     colonize: { attractionRadius: 6, segmentLength: 0.25,
 *                 tropism: [0,1,0], tropismWeight: 0.4 },
 *     seed: 42,
 *   });
 *   const leaf = Mesh.leafCard('oval', { width: 0.1, length: 0.18 });
 *   const foliage = Mesh.scatterLeaves(segments, leaf, { perUnitLength: 30 });
 */
Mesh.tree = function(opts) {};


// -----------------------------------------------------------------------------
// L-system rewriting
// -----------------------------------------------------------------------------
//
// String-rule stochastic L-system. Produces a flat module sequence — turtle
// interpretation and geometry building are caller concerns. Bracket symbols
// `[` and `]` pass through unchanged when no rule matches them.
//
// Module shape: { symbol: string, params: number[] }
//
// Compact text form: `F(1.0)[+(25)F]F`. A symbol is any non-whitespace char
// other than `(`, `)`, `,`. Each symbol may be followed by a parenthesized
// comma-separated list of floats. Whitespace is ignored.

/**
 * Parse a compact L-system string into a module list. Returns [] on parse
 * error.
 *
 * @param {string} text
 * @returns {Array<{symbol: string, params: number[]}>}
 */
Mesh.parseLSystem = function(text) {};

/**
 * Interpret a flat L-system module sequence as a 3D turtle and return the
 * branch segments it traces. The result is a `BranchSegment[]` matching the
 * shape produced by `Mesh.spaceColonize`, so it drops straight into
 * `Mesh.thickenBranches` / `Mesh.meshBranches` / `Mesh.scatterLeaves`.
 *
 * Recognised symbols (numeric params override defaults):
 *   F(len?, r?)    forward; emit a segment from current pos to pos+heading*len.
 *                  Optional second param sets THIS segment's radius (the
 *                  active radius is unchanged).
 *   G(len?), f(len?)
 *                  forward without emitting a segment.
 *   +(deg?), -(deg?)   yaw around `up`
 *   &(deg?), ^(deg?)   pitch around the left axis (heading × up)
 *   \(deg?), /(deg?)   roll around `heading`
 *   |              turn 180° around `up`
 *   !(r)           set the active radius for subsequent F segments
 *   [, ]           push / pop turtle state (branching)
 *
 * Numeric params on rotation symbols are degrees; the `angle` option below
 * is in radians and is used when a rotation symbol has no parameter.
 * Unknown symbols are silently ignored so user grammars can carry their own
 * annotations.
 *
 * @param {Array<{symbol: string, params: number[]}>} modules
 * @param {Object} [opts]
 * @param {number} [opts.stepLength=0.1]   default forward step (when F has no length)
 * @param {number} [opts.angle=0.4363]     default rotation in radians (~25°)
 * @param {number} [opts.radius=0.01]      default segment radius
 * @param {number[]} [opts.position=[0,0,0]]
 * @param {number[]} [opts.heading=[0,1,0]]    initial forward direction
 * @param {number[]} [opts.up=[0,0,1]]         initial roll reference
 * @returns {Array<BranchSegment>}
 *
 * @example
 *   const mods = Mesh.parseLSystem('!(0.04)F[+(35)F!(0.02)F][-(35)F!(0.02)F]F');
 *   const segs = Mesh.lsystemToBranches(mods, { stepLength: 0.5 });
 *   const trunk = Mesh.meshBranches(segs, 6);
 */
Mesh.lsystemToBranches = function(modules, opts) {};

/**
 * Stochastic string-rule L-system rewriter. For parametric rules with
 * conditions, build geometry directly from `Mesh.spaceColonize` /
 * `Mesh.meshBranches` instead.
 */
class LSystem {
  /**
   * @param {string} [axiom]   compact-form starting word (parsed via parseLSystem)
   */
  constructor(axiom) {}

  /**
   * Set the axiom from a compact string. Returns this.
   * @param {string} text
   */
  setAxiom(text) {}

  /**
   * Add a production rule. Multiple rules sharing a predecessor are selected
   * stochastically by `weight`. Returns this.
   * @param {string} predecessor   single-character symbol
   * @param {string} successor     compact-form replacement string
   * @param {number} [weight=1]
   */
  addRule(predecessor, successor, weight) {}

  /**
   * Run `iterations` rewrite passes. Deterministic given `seed`.
   * @param {number} iterations
   * @param {number} [seed=0]
   * @returns {string}   compact-form serialization of the derived module list
   */
  derive(iterations, seed) {}

  /**
   * Same as `derive` but returns the parsed module list directly instead of a
   * compact-form string — saves a re-parse if you need the structured form.
   * @param {number} iterations
   * @param {number} [seed=0]
   * @returns {Array<{symbol: string, params: number[]}>}
   */
  deriveModules(iterations, seed) {}
}
