// =============================================================================
// bro Mesh API Reference
// =============================================================================
//
// The Mesh class is a standalone global that wraps the bromesh C++ library.
// It provides mesh generation, file I/O, manipulation, analysis, CSG boolean
// operations, isosurface extraction, UV mapping, and optimization.
//
// Mesh objects hold geometry data (positions, normals, UVs, colors, indices)
// and can be used independently or fed into the scene graph for rendering.
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


  // --- Static: Primitives ---------------------------------------------------

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


  // --- Static: CSG ----------------------------------------------------------

  /**
   * Boolean union of two meshes.
   * @param {Mesh} a
   * @param {Mesh} b
   * @returns {Mesh}
   */
  static union(a, b) {}

  /**
   * Boolean difference (a - b).
   * @param {Mesh} a
   * @param {Mesh} b
   * @returns {Mesh}
   */
  static subtract(a, b) {}

  /**
   * Boolean intersection of two meshes.
   * @param {Mesh} a
   * @param {Mesh} b
   * @returns {Mesh}
   */
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
   * Merge multiple meshes into one.
   * @param {Mesh[]} meshes
   * @returns {Mesh}
   */
  static merge(meshes) {}


  // --- Static: Isosurface ---------------------------------------------------

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
   * Greedy meshing for voxel data.
   * @param {Uint8Array} voxels - 3D grid, 0 = empty, nonzero = palette index
   * @param {number} gridX
   * @param {number} gridY
   * @param {number} gridZ
   * @param {number} [cellSize=1]
   * @param {Float32Array} [palette] - RGBA per material
   * @param {number} [paletteCount]
   * @returns {Mesh}
   */
  static greedyMesh(voxels, gridX, gridY, gridZ, cellSize, palette, paletteCount) {}


  // --- Static: File I/O (load) ----------------------------------------------

  /**
   * Load a glTF/GLB file.
   * @param {string} path
   * @returns {{ meshes: Mesh[] }}
   */
  static loadGLTF(path) {}

  /**
   * Load a Wavefront OBJ file.
   * @param {string} path
   * @returns {Mesh}
   */
  static loadOBJ(path) {}

  /**
   * Load an FBX file.
   * @param {string} path
   * @returns {Mesh[]}
   */
  static loadFBX(path) {}

  /**
   * Load a PLY file.
   * @param {string} path
   * @returns {Mesh}
   */
  static loadPLY(path) {}

  /**
   * Load a binary STL file.
   * @param {string} path
   * @returns {Mesh}
   */
  static loadSTL(path) {}

  /**
   * Load a MagicaVoxel VOX file.
   * @param {string} path
   * @returns {{ sizeX: number, sizeY: number, sizeZ: number, voxels: Uint8Array, palette: Float32Array }}
   */
  static loadVOX(path) {}

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


  // --- Transform (mutating, return this) ------------------------------------

  /**
   * Translate all vertices.
   * @param {number} dx
   * @param {number} dy
   * @param {number} dz
   * @returns {Mesh} this
   */
  translate(dx, dy, dz) {}

  /**
   * Scale all vertices. One arg = uniform, three = per-axis.
   * Non-uniform scale triggers normal recomputation.
   * @param {number} sx
   * @param {number} [sy=sx]
   * @param {number} [sz=sx]
   * @returns {Mesh} this
   */
  scale(sx, sy, sz) {}

  /**
   * Rotate all vertices around an axis.
   * @param {number} axisX
   * @param {number} axisY
   * @param {number} axisZ
   * @param {number} angleRadians
   * @returns {Mesh} this
   */
  rotate(axisX, axisY, axisZ, angleRadians) {}

  /** Center the mesh at the origin. @returns {Mesh} this */
  center() {}

  /**
   * Mirror across an axis. Flips winding order.
   * @param {number} axis - 0=X, 1=Y, 2=Z
   * @returns {Mesh} this
   */
  mirror(axis) {}

  /**
   * Apply a 4x4 column-major transformation matrix.
   * @param {Float32Array} matrix - 16 floats
   * @returns {Mesh} this
   */
  transform(matrix) {}


  // --- Normals --------------------------------------------------------------

  /** Recompute smooth normals in-place. @returns {Mesh} this */
  computeNormals() {}

  /** Compute flat normals (duplicates vertices). @returns {Mesh} new mesh */
  computeFlatNormals() {}

  /** Compute tangents. @returns {Float32Array} 4 floats per vertex */
  computeTangents() {}


  // --- Simplification (mutating, return this) -------------------------------

  /**
   * Simplify mesh by target ratio.
   * @param {number} targetRatio - 0.0 to 1.0
   * @param {number} [targetError=0.01]
   * @returns {Mesh} this
   */
  simplify(targetRatio, targetError) {}

  /**
   * Simplify preserving UV and normal quality.
   * @param {number} targetRatio
   * @param {number} [targetError=0.01]
   * @param {number} [uvWeight=1.0]
   * @param {number} [normalWeight=0.5]
   * @returns {Mesh} this
   */
  simplifyWithAttributes(targetRatio, targetError, uvWeight, normalWeight) {}

  /**
   * Simplify to a specific triangle count.
   * @param {number} targetTriangles
   * @param {number} [targetError=0.01]
   * @returns {Mesh} this
   */
  simplifyToTriangleCount(targetTriangles, targetError) {}

  /**
   * Generate an array of LOD meshes at the given ratios.
   * @param {Float32Array} ratios - e.g. [0.5, 0.25, 0.1]
   * @returns {Mesh[]}
   */
  generateLODChain(ratios) {}


  // --- Subdivision (mutating, return this) ----------------------------------

  /** @param {number} [iterations=1] @returns {Mesh} this */
  subdivideLoop(iterations) {}

  /** @param {number} [iterations=1] @returns {Mesh} this */
  subdivideCatmullClark(iterations) {}

  /** @param {number} [iterations=1] @returns {Mesh} this */
  subdivideMidpoint(iterations) {}


  // --- Smoothing (mutating, return this) ------------------------------------

  /**
   * Laplacian smoothing.
   * @param {number} [lambda=0.5]
   * @param {number} [iterations=1]
   * @returns {Mesh} this
   */
  smoothLaplacian(lambda, iterations) {}

  /**
   * Taubin smoothing (volume-preserving).
   * @param {number} [lambda=0.5]
   * @param {number} [mu=-0.53]
   * @param {number} [iterations=1]
   * @returns {Mesh} this
   */
  smoothTaubin(lambda, mu, iterations) {}


  // --- Optimization (mutating, return this) ---------------------------------

  /** Reorder indices for GPU vertex cache. @returns {Mesh} this */
  optimizeVertexCache() {}

  /** Reorder vertices for sequential access. @returns {Mesh} this */
  optimizeVertexFetch() {}

  /**
   * Reorder for reduced overdraw.
   * @param {number} [threshold=1.05]
   * @returns {Mesh} this
   */
  optimizeOverdraw(threshold) {}


  // --- Analysis -------------------------------------------------------------

  /**
   * Compute axis-aligned bounding box.
   * @returns {{ min: number[], max: number[], centerX, centerY, centerZ, extentX, extentY, extentZ }}
   */
  computeBBox() {}

  /** @returns {boolean} whether the mesh is manifold */
  isManifold() {}

  /** @returns {number} signed volume (manifold meshes only) */
  computeVolume() {}

  /**
   * Cast a ray against the mesh.
   * @param {number[]} origin - [x, y, z]
   * @param {number[]} direction - [x, y, z]
   * @param {number} [maxDistance=0] - 0 = unlimited
   * @returns {?{ distance, position: number[], normal: number[], triangleIndex, baryU, baryV, baryW }}
   */
  raycast(origin, direction, maxDistance) {}

  /**
   * Cast a ray and return all hits.
   * @param {number[]} origin
   * @param {number[]} direction
   * @param {number} [maxDistance=0]
   * @returns {Object[]} array of hit objects
   */
  raycastAll(origin, direction, maxDistance) {}

  /**
   * Fast boolean ray intersection test.
   * @param {number[]} origin
   * @param {number[]} direction
   * @param {number} [maxDistance=0]
   * @returns {boolean}
   */
  raycastTest(origin, direction, maxDistance) {}

  /**
   * Find the closest point on the mesh surface to a given point.
   * @param {number[]} point - [x, y, z]
   * @returns {?{ distance, position: number[], normal: number[], triangleIndex, baryU, baryV, baryW }}
   */
  closestPoint(point) {}

  /** @returns {boolean} */
  hasSelfIntersections() {}

  /** @returns {{ triA: number, triB: number }[]} */
  findSelfIntersections() {}

  /**
   * Test whether this mesh intersects another.
   * @param {Mesh} other
   * @returns {boolean}
   */
  intersectsMesh(other) {}


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


  // --- File I/O (save) ------------------------------------------------------

  /** @param {string} path @returns {boolean} */ saveGLTF(path) {}
  /** @param {string} path @returns {boolean} */ saveOBJ(path) {}
  /** @param {string} path @returns {boolean} */ savePLY(path) {}
  /** @param {string} path @returns {boolean} */ saveSTL(path) {}
}
