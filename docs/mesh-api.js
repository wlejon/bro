// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Mesh API Reference
 * =============================================================================
 *
 * The bromesh C++ library is exposed via these classes and namespaces:
 * Mesh, MeshBVH, ProgressiveMesh, PolyMesh, CapsuleField, LSystem.
 * @typedef {Object} MeshOptions
 * @property {Float32Array} [positions]
 * @property {Float32Array} [normals]
 * @property {Float32Array} [uvs]
 * @property {Float32Array} [colors]
 * @property {Uint32Array} [indices]
 */

/**
 * @typedef {Object} MeshBVHIntersectResult
 * @property {boolean} [hit]
 * @property {number} [distance]
 * @property {number} [triangle]
 * @property {Array<number>} [point]
 * @property {Array<number>} [normal]
 * @property {Array<number>} [uv]
 * @property {Array<number>} [barycentric]
 */

/**
 * @typedef {Object} MeshletGroupResult
 * @property {number} [meshletCount]
 * @property {ArrayBuffer} [meshlets]
 * @property {Uint32Array} [vertices]
 * @property {Uint8Array} [triangles]
 */

/**
 * @typedef {Object} MeshUVQualityResult
 * @property {number} [coverage]
 * @property {number} [minAreaRatio]
 * @property {number} [maxAreaRatio]
 * @property {number} [avgStretch]
 * @property {number} [maxStretch]
 * @property {number} [avgAngleError]
 * @property {number} [overlaps]
 */

/**
 * @typedef {Object} MeshUVDistortionResult
 * @property {number} [stretch]
 * @property {number} [areaDistortion]
 * @property {number} [angleDistortion]
 */

/**
 * @typedef {Object} MeshRepairStats
 * @property {number} [nonManifoldEdgesFixed]
 * @property {number} [degenerateTrianglesRemoved]
 * @property {number} [selfIntersectionsResolved]
 * @property {number} [holesClosed]
 */

/**
 * @typedef {Object} MeshBakeTransferOptions
 * @property {number} [maxDistance]
 * @property {number} [uvScale]
 * @property {Array<number>} [uvOffset]
 * @property {number} [sampleQuality]
 * @property {string} [normalSpace]
 */

/**
 * @typedef {Object} MeshTreeOptions
 * @property {Array<number>} [base]
 * @property {Array<number>} [canopyCenter]
 * @property {number} [canopyRadius]
 * @property {number} [attractorCount]
 * @property {number} [sides]
 * @property {number} [leafRadius]
 * @property {number} [pipeExp]
 * @property {number} [seed]
 * @property {MeshSpaceColonizationOptions} [colonize] -  Space-colonization tuning for the skeleton pass.
 */

/**
 *  Result of Mesh.tree(): the thickened skeleton plus its swept branch mesh.
 * @typedef {Object} MeshTreeResult
 * @property {Array<MeshBranchSegment>} [segments]
 * @property {Mesh} [branches]
 */

/**
 * @typedef {Object} MeshSweepOptions
 * @property {boolean} [closeProfile]
 * @property {boolean} [capStart]
 * @property {boolean} [capEnd]
 * @property {boolean} [miterJoints]
 * @property {*} [profileScale] -  Per-ring profile scale: a number, or one entry per path point.
 * @property {*} [twist] -  Per-ring twist in radians: a number, or one entry per path point.
 */

/**
 * @typedef {Object} MeshTubeOptions
 * @property {boolean} [capStart]
 * @property {boolean} [capEnd]
 * @property {boolean} [miterJoints]
 */

/**
 * @typedef {Object} MeshBladeStripOptions
 * @property {number} [width]
 * @property {number} [thickness]
 * @property {boolean} [capStart]
 * @property {boolean} [capEnd]
 * @property {boolean} [miterJoints]
 * @property {*} [profileScale]
 * @property {*} [twist]
 */

/**
 * @typedef {Object} MeshBladePathOptions
 * @property {Array<number>} [base]
 * @property {Array<number>} [tipDir]
 * @property {number} [length]
 * @property {number} [bend]
 * @property {number} [lift]
 * @property {number} [segments]
 */

/**
 *  Capsule obstacle for CapsuleField: the segment a→b swept by `radius`.
 * @typedef {Object} MeshCapsule
 * @property {Array<number>} [a]
 * @property {Array<number>} [b]
 * @property {number} [radius]
 * @property {number} [tag] -  Optional identity used by the `excludeTag` query parameters.
 */

/**
 *  Sphere obstacle / keep-out volume.
 * @typedef {Object} MeshSphere
 * @property {Array<number>} [center]
 * @property {number} [radius]
 * @property {number} [tag]
 */

/**
 * @typedef {Object} MeshCapsuleFieldNearest
 * @property {Array<number>} [point]
 * @property {Array<number>} [normal]
 * @property {number} [distance]
 * @property {number} [tag]
 */

/**
 * @typedef {Object} MeshAnchorPackOptions
 * @property {number} [minSpacing]
 * @property {number} [minObstacleDistance]
 * @property {number} [maxCount]
 * @property {number} [seed]
 * @property {*} [avoid] -  CapsuleField the anchors must clear by `minObstacleDistance`.
 * @property {Array<MeshSphere>} [keepOut]
 */

/**
 * @typedef {Object} MeshTurtleOptions
 * @property {number} [stepLength]
 * @property {number} [angle] -  Turn angle in radians.
 * @property {number} [radius]
 * @property {Array<number>} [position]
 * @property {Array<number>} [heading]
 * @property {Array<number>} [up]
 */

/**
 * @typedef {Object} MeshLeafCardOptions
 * @property {number} [width]
 * @property {number} [length]
 * @property {number} [bend]
 * @property {number} [curl]
 * @property {boolean} [stemOffset]
 * @property {number} [cup]
 * @property {number} [widthSegments]
 * @property {number} [lengthSegments]
 * @property {boolean} [fullUV]
 * @property {boolean} [shapedSilhouette]
 */

/**
 * @typedef {Object} MeshFlowerOptions
 * @property {number} [petalCount]
 * @property {string} [petalShape]
 * @property {number} [petalLength]
 * @property {number} [petalWidth]
 * @property {number} [petalCurl]
 * @property {number} [petalBend]
 * @property {number} [layers]
 * @property {number} [layerTwist]
 * @property {number} [centerRadius]
 * @property {number} [centerHeight]
 * @property {number} [outerTilt]
 * @property {number} [innerTilt]
 * @property {number} [layerScaleFalloff]
 * @property {number} [outerYLift]
 * @property {number} [innerYLift]
 * @property {number} [petalCup]
 * @property {boolean} [shapedPetals]
 * @property {Array<number>} [centerColor]
 */

/**
 * @typedef {Object} MeshBezierSweepOptions
 * @property {number} [samples]
 * @property {boolean} [capStart]
 * @property {boolean} [capEnd]
 * @property {boolean} [closeProfile]
 * @property {boolean} [miterJoints]
 * @property {*} [profileScale]
 * @property {*} [twist]
 */

/**
 * @typedef {Object} MeshSpaceColonizationOptions
 * @property {number} [attractionRadius]
 * @property {number} [killRadius]
 * @property {number} [segmentLength]
 * @property {number} [maxIterations]
 * @property {number} [tropismWeight]
 * @property {Array<number>} [tropism]
 * @property {*} [obstacles]
 * @property {number} [obstacleClearance]
 * @property {number} [obstacleSteer]
 */

/**
 *  One segment of a branch skeleton (spaceColonize / lsystemToBranches / tree).
 * @typedef {Object} MeshBranchSegment
 * @property {number} [parent] -  Index of the parent segment, -1 at a root.
 * @property {Array<number>} [from]
 * @property {Array<number>} [to]
 * @property {number} [radius] -  Radius at `from`; 0 until thickenBranches assigns the pipe model.
 * @property {number} [depth]
 */

/**
 * @typedef {Object} MeshLeafPlacementOptions
 * @property {number} [maxRadius]
 * @property {number} [minDepth]
 * @property {boolean} [terminalOnly]
 * @property {number} [perUnitLength]
 * @property {number} [densityFalloff]
 * @property {number} [upBias]
 * @property {number} [tiltJitter]
 * @property {number} [rollJitter]
 * @property {number} [baseScale]
 * @property {number} [scaleJitter]
 * @property {number} [scaleByRadius]
 * @property {number} [dedupRadius]
 * @property {number} [seed]
 * @property {Array<number>} [densityWeight]
 * @property {*} [avoid]
 * @property {number} [obstacleClearance]
 * @property {number} [obstaclePushout]
 * @property {Array<Object>} [keepOut]
 */

/**
 * @typedef {Object} MeshPlacedLeaves
 * @property {number} [count]
 * @property {Float32Array} [transforms]
 * @property {Float32Array} [branchRadius]
 * @property {Int32Array} [branchDepth]
 */

/**
 * @typedef {Object} MeshBlobOptions
 * @property {number} [radius]
 * @property {number} [seed]
 * @property {number} [nsub]
 * @property {*} [scale]
 * @property {*} [center]
 */

/**
 * @typedef {Object} MeshDracoDecodedAttribute
 * @property {string} [type]
 * @property {number} [uniqueId]
 * @property {number} [components]
 * @property {number} [count]
 * @property {string} [kind]
 * @property {ArrayBufferView} [data]
 */

/**
 * @typedef {Object} MeshDracoDecoded
 * @property {Float32Array} [positions]
 * @property {Float32Array} [normals]
 * @property {Float32Array} [uvs]
 * @property {Float32Array} [colors]
 * @property {Uint32Array} [indices]
 * @property {Array<MeshDracoDecodedAttribute>} [attributes]
 * @property {Mesh} [mesh]
 */

/**
 * @typedef {Object} MeshDracoEncodeOptions
 * @property {number} [positionBits]
 * @property {number} [normalBits]
 * @property {number} [uvBits]
 * @property {number} [colorBits]
 * @property {number} [genericBits]
 * @property {number} [compressionLevel]
 * @property {boolean} [sequential]
 */

/**
 * @typedef {Object} MeshExtrudeFaceResult
 * @property {Int32Array} [dupVerts]
 * @property {Int32Array} [bridgeFaces]
 * @property {Int32Array} [bridgeAdjGroup] -  Per bridge face, the group across its boundary edge (-1 at a mesh boundary).
 * @property {number} [backFace] -  The back-face copy that closes the slab, or -1 without one.
 */

/**
 * @typedef {Object} MeshInsetFaceResult
 * @property {number} [innerFace] -  The new interior face, or -1 when the inset was refused.
 * @property {Int32Array} [innerVerts]
 * @property {Int32Array} [bridgeFaces]
 */

/**
 *  PolyMesh.tessellate(): flat-shaded triangles, one normal per face.
 * @typedef {Object} MeshTessellation
 * @property {Float32Array} [positions]
 * @property {Float32Array} [normals]
 * @property {Uint32Array} [indices]
 * @property {Int32Array} [triToFace] -  Source face of each triangle.
 * @property {Int32Array} [triToGroup] -  Group of each triangle's source face.
 */

/**
 * @typedef {Object} MeshPolyValidation
 * @property {boolean} [valid] -  Structurally sound: every face closes, no dangling links.
 * @property {boolean} [isClosed] -  Every half-edge has a twin.
 * @property {number} [boundaryHalfEdges]
 * @property {Array<string>} [errors]
 */

/**
 *  A MagicaVoxel grid: 0 = empty, else an index into `palette` (RGBA x 256).
 * @typedef {Object} MeshVoxData
 * @property {number} [sizeX]
 * @property {number} [sizeY]
 * @property {number} [sizeZ]
 * @property {Uint8Array} [voxels]
 * @property {Float32Array} [palette]
 */

/**
 * A Gaussian splat cloud: the object `scene.createGaussianSplat({ cloud })`
 * takes. Per-splat streams, `count` splats: `positions` xyz, `scales` xyz
 * (linear std-dev), `rotations` xyzw unit quaternion, `opacities` [0,1], `sh`
 * coefficient-major spherical harmonics with 3 * (shDegree + 1)^2 floats
 * per splat.
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
 *  Options for `Mesh.reconstruct`.
 * @typedef {Object} MeshReconstructOptions
 * @property {number} [gridResolution] -  Voxel grid resolution along the longest axis (default 64).
 * @property {number} [supportRadius] -  Influence radius per point; 0 (the default) derives it from point density.
 * @property {number} [isoLevel] -  Threshold for surface extraction (default 0.5).
 */

/**
 *  Everything a glTF file holds; `meshSkeleton[i]` / `animationSkeleton[i]` index `skeletons`.
 * @typedef {Object} MeshGltfScene
 * @property {Array<Mesh>} [meshes]
 * @property {Array<SkinData>} [skins]
 * @property {Array<Skeleton>} [skeletons]
 * @property {Array<SkeletalAnimation>} [animations]
 * @property {Array<number>} [meshSkeleton]
 * @property {Array<number>} [animationSkeleton]
 */

/**
 * @typedef {Object} MeshLSystemModule
 * @property {string} [symbol]
 * @property {Array<number>} [params]
 */

/**
 * @typedef {Object} MeshConvexDecompParams
 * @property {number} [maxHulls]
 * @property {number} [maxVerticesPerHull]
 * @property {number} [resolution]
 * @property {number} [minVolumePerHull]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class Mesh {

  /**
   * @param {MeshOptions} [opts]
   */
  constructor(opts) {}

  /**
   * @param {number} [halfW]
   * @param {number} [halfH]
   * @param {number} [halfD]
   * @returns {Mesh}
   */
  static box(halfW, halfH, halfD) {}

  /**
   * @param {number} [radius]
   * @param {number} [segments]
   * @param {number} [rings]
   * @returns {Mesh}
   */
  static sphere(radius, segments, rings) {}

  /**
   * @param {number} [radius]
   * @param {number} [halfHeight]
   * @param {number} [segments]
   * @returns {Mesh}
   */
  static cylinder(radius, halfHeight, segments) {}

  /**
   * @param {number} [radius]
   * @param {number} [halfHeight]
   * @param {number} [segments]
   * @returns {Mesh}
   */
  static capsule(radius, halfHeight, segments) {}

  /**
   * @param {number} [radius]
   * @param {number} [height]
   * @param {number} [segments]
   * @returns {Mesh}
   */
  static cone(radius, height, segments) {}

  /**
   * @param {number} [halfW]
   * @param {number} [halfH]
   * @param {number} [segW]
   * @param {number} [segH]
   * @returns {Mesh}
   */
  static plane(halfW, halfH, segW, segH) {}

  /**
   * @param {number} [radius]
   * @param {number} [tubeRadius]
   * @param {number} [segments]
   * @param {number} [tubeSegments]
   * @returns {Mesh}
   */
  static torus(radius, tubeRadius, segments, tubeSegments) {}

  /**
   * @param {number} [radius]
   * @returns {Mesh}
   */
  static icosahedron(radius) {}

  /**
   * @param {number} [radius]
   * @returns {Mesh}
   */
  static dodecahedron(radius) {}

  /**
   * @param {number} [radius]
   * @returns {Mesh}
   */
  static octahedron(radius) {}

  /**
   * @param {number} [radius]
   * @returns {Mesh}
   */
  static tetrahedron(radius) {}

  /**
   * @param {number} [radius]
   * @param {number} [segments]
   * @returns {Mesh}
   */
  static disk(radius, segments) {}

  /**
   * Circular-cross-section sweep along `path` (Float32Array(3N) or
   * [[x,y,z], ...], at least 2 points). `radius` is a constant number or a
   * per-point list the same length as `path`; `sides` is the ring
   * resolution (>= 3, default 8).
   *
   * @param {Array<Array<number>>} path
   * @param {*} [radius]
   * @param {number} [sides]
   * @param {MeshTubeOptions} [opts]
   * @returns {Mesh}
   */
  static tube(path, radius, sides, opts) {}

  /**
   * Read a mesh from a file. A relative path resolves the way `fs.*`
   * resolves it. A file that cannot be read gives an empty Mesh. An FBX is
   * a scene, so `loadFBX` gives every mesh in it; a `.vox` is a voxel grid,
   * not a mesh (`loadVOX` gives the grid, `Mesh.greedyMesh` meshes it);
   * glTF gives the whole scene: meshes, skins, skeletons, animations.
   *
   * @param {string} path
   * @returns {Mesh}
   */
  static loadOBJ(path) {}

  /**
   * @param {string} path
   * @returns {Mesh}
   */
  static loadPLY(path) {}

  /**
   * @param {string} path
   * @returns {Mesh}
   */
  static loadSTL(path) {}

  /**
   * @param {string} path
   * @returns {Array<Mesh>}
   */
  static loadFBX(path) {}

  /**
   * @param {string} path
   * @returns {MeshVoxData}
   */
  static loadVOX(path) {}

  /**
   * @param {string} path
   * @returns {MeshGltfScene}
   */
  static loadGLTF(path) {}

  /**
   *  A Gaussian splat `.ply` is a cloud, not a mesh; an unreadable file gives an empty cloud (`count` 0).
   *
   * @param {string} path
   * @returns {MeshSplatCloud}
   */
  static loadSplatPLY(path) {}

  /**
   *  Write a splat cloud (the `loadSplatPLY` / `bro.triposplat` shape); true when the file was written.
   *
   * @param {string} path
   * @param {MeshSplatCloud} cloud
   * @returns {boolean}
   */
  static saveSplatPLY(path, cloud) {}

  /**
   * @param {Array<Mesh>} meshes
   * @returns {Mesh}
   */
  static merge(meshes) {}

  /**
   * Triangulate a simple 2D polygon (`outer` flat x,y,..., CCW for a +Z
   * face) with optional `holes` (each flat x,y,..., wound CW) into a mesh
   * in the plane z = `z`. Degenerate input gives an empty Mesh.
   *
   * @param {Array<number>} outer
   * @param {Array<Array<number>>} [holes]
   * @param {number} [z]
   * @returns {Mesh}
   */
  static polygon2D(outer, holes, z) {}

  /**
   * Triangulate a planar 3D polygon (`outer` flat x,y,z,..., lying on the
   * plane with unit `normal`) with optional `holes`; the mesh reuses the
   * input positions and fills normals with `normal`.
   *
   * @param {Array<number>} outer
   * @param {Array<Array<number>>} holes
   * @param {Array<number>} normal
   * @returns {Mesh}
   */
  static polygon3D(outer, holes, normal) {}

  /**
   *  Implicit-surface reconstruction of an oriented point cloud (a Mesh with positions + normals; indices ignored).
   *
   * @param {Mesh} pointCloud
   * @param {MeshReconstructOptions} [opts]
   * @returns {Mesh}
   */
  static reconstruct(pointCloud, opts) {}

  /**
   * @param {Array<number>} values
   * @param {number} dimX
   * @param {number} dimY
   * @param {number} dimZ
   * @param {number} isoLevel
   * @returns {Mesh}
   */
  static marchingCubes(values, dimX, dimY, dimZ, isoLevel) {}

  /**
   * @param {Array<number>} values
   * @param {number} dimX
   * @param {number} dimY
   * @param {number} dimZ
   * @param {number} isoLevel
   * @returns {Mesh}
   */
  static surfaceNets(values, dimX, dimY, dimZ, isoLevel) {}

  /**
   * @param {Array<number>} values
   * @param {number} dimX
   * @param {number} dimY
   * @param {number} dimZ
   * @param {number} isoLevel
   * @returns {Mesh}
   */
  static dualContouring(values, dimX, dimY, dimZ, isoLevel) {}

  /**
   * Extrude a closed 2D `profile` (Float32Array(2N) or [[x,y], ...]) along a
   * 3D `path` (Float32Array(3N) or [[x,y,z], ...]).
   *
   * @param {Array<Array<number>>} profile
   * @param {Array<Array<number>>} path
   * @param {MeshSweepOptions} [opts]
   * @returns {Mesh}
   */
  static sweep(profile, path, opts) {}

  /**
   * @param {Array<Array<number>>} controlPoints
   * @param {Array<Array<number>>} profile
   * @param {MeshBezierSweepOptions} [opts]
   * @returns {Mesh}
   */
  static bezierSweep(controlPoints, profile, opts) {}

  /**
   * @param {*} shape
   * @param {MeshLeafCardOptions} [opts]
   * @returns {Mesh}
   */
  static leafCard(shape, opts) {}

  /**
   * @param {MeshFlowerOptions} [opts]
   * @returns {Mesh}
   */
  static flower(opts) {}

  /**
   *  Sweep a 4-vertex diamond profile along `path`: grass / fern / succulent blades.
   *
   * @param {Array<Array<number>>} path
   * @param {MeshBladeStripOptions} [opts]
   * @returns {Mesh}
   */
  static bladeStrip(path, opts) {}

  /**
   *  Quadratic-Bézier blade spine as [[x,y,z], ...], consumable by bladeStrip / sweep.
   *
   * @param {MeshBladePathOptions} [opts]
   * @returns {Array<Array<number>>}
   */
  static bladePath(opts) {}

  /**
   * @param {MeshBlobOptions} [opts]
   * @returns {Mesh}
   */
  static blob(opts) {}

  /**
   * @param {Array<Array<number>>} attractors
   * @param {Array<Array<number>>} seedPoints
   * @param {Array<number>} initialDirection
   * @param {MeshSpaceColonizationOptions} [opts]
   * @returns {Array<MeshBranchSegment>}
   */
  static spaceColonize(attractors, seedPoints, initialDirection, opts) {}

  /**
   * @param {Array<MeshBranchSegment>} segments
   * @param {number} [leafRadius]
   * @param {number} [pipeExp]
   * @returns {Array<MeshBranchSegment>}
   */
  static thickenBranches(segments, leafRadius, pipeExp) {}

  /**
   * @param {Array<MeshBranchSegment>} segments
   * @param {number} [sides]
   * @returns {Mesh}
   */
  static meshBranches(segments, sides) {}

  /**
   * @param {Array<MeshBranchSegment>} segments
   * @param {MeshLeafPlacementOptions} [opts]
   * @returns {MeshPlacedLeaves}
   */
  static placeLeavesOnBranches(segments, opts) {}

  /**
   * @param {Array<MeshBranchSegment>} segments
   * @param {Mesh} leaf
   * @param {MeshLeafPlacementOptions} [opts]
   * @returns {Mesh}
   */
  static scatterLeaves(segments, leaf, opts) {}

  /**
   *  spaceColonize → thickenBranches → meshBranches in one call.
   *
   * @param {MeshTreeOptions} [opts]
   * @returns {MeshTreeResult}
   */
  static tree(opts) {}

  /**
   * @param {string} text
   * @returns {Array<MeshLSystemModule>}
   */
  static parseLSystem(text) {}

  /**
   *  Greedy spaced-anchor picker; returns the accepted candidate indices in acceptance order.
   *
   * @param {Array<Array<number>>} candidates
   * @param {MeshAnchorPackOptions} [opts]
   * @returns {Int32Array}
   */
  static packAnchors(candidates, opts) {}

  /**
   *  Turtle-interpret a module stream (or an L-system string) into a branch skeleton.
   *
   * @param {Array<MeshLSystemModule>} modules
   * @param {MeshTurtleOptions} [opts]
   * @returns {Array<MeshBranchSegment>}
   */
  static lsystemToBranches(modules, opts) {}

  /**
   * @param {Array<MeshCapsule>} [capsules]
   * @param {Array<MeshSphere>} [spheres]
   * @param {number} [cellSize]
   * @returns {CapsuleField}
   */
  static capsuleField(capsules, spheres, cellSize) {}

  /**
   *  CapsuleField whose capsules carry the segment index as `tag`, so placement can exclude a leaf's own branch.
   *
   * @param {Array<MeshBranchSegment>} segments
   * @param {number} [radiusScale]
   * @param {Array<MeshSphere>} [extraSpheres]
   * @returns {CapsuleField}
   */
  static capsuleFieldFromSegments(segments, radiusScale, extraSpheres) {}

  /**
   * @param {ArrayBufferView} bytes
   * @returns {MeshDracoDecoded}
   */
  static decodeDraco(bytes) {}

  /**
   * @param {Object} meshData
   * @param {MeshDracoEncodeOptions} [opts]
   * @returns {ArrayBuffer}
   */
  static encodeDraco(meshData, opts) {}

  /**
   * @type {Float32Array}
   */
  positions;

  /**
   * @type {Float32Array}
   */
  normals;

  /**
   * @type {Float32Array}
   */
  uvs;

  /**
   * @type {Float32Array}
   */
  colors;

  /**
   * @type {Uint32Array}
   */
  indices;

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @readonly
   * @type {number}
   */
  triangleCount;

  /**
   * @readonly
   * @type {boolean}
   */
  hasNormals;

  /**
   * @readonly
   * @type {boolean}
   */
  hasUVs;

  /**
   * @readonly
   * @type {boolean}
   */
  hasColors;

  /**
   * @readonly
   * @type {boolean}
   */
  empty;

  /**
   * @returns {Mesh}
   */
  clone() {}

  /**
   * @param {number} dx
   * @param {number} dy
   * @param {number} dz
   * @returns {Mesh}
   */
  translate(dx, dy, dz) {}

  /**
   * @param {number} sx
   * @param {number} [sy]
   * @param {number} [sz]
   * @returns {Mesh}
   */
  scale(sx, sy, sz) {}

  /**
   * @param {number} ax
   * @param {number} ay
   * @param {number} az
   * @param {number} angle
   * @returns {Mesh}
   */
  rotate(ax, ay, az, angle) {}

  /**
   * @returns {Mesh}
   */
  center() {}

  /**
   * @param {number} size
   * @returns {Mesh}
   */
  fitToBox(size) {}

  /**
   * @param {Array<number>} matrix
   * @returns {Mesh}
   */
  transform(matrix) {}

  /**
   * @param {SkinData} skin
   * @param {Array<number>} matrices
   * @returns {Mesh}
   */
  applySkinning(skin, matrices) {}

  /**
   * @param {Mesh} target
   * @param {number} weight
   * @returns {Mesh}
   */
  applyMorphTarget(target, weight) {}

  /**
   * @param {number} [creaseAngle]
   * @returns {Mesh}
   */
  computeNormals(creaseAngle) {}

  /**
   * @returns {Mesh}
   */
  invertNormals() {}

  /**
   * @returns {Mesh}
   */
  flipFaces() {}

  /**
   * @param {number} [threshold]
   * @returns {Mesh}
   */
  weld(threshold) {}

  /**
   * @param {number} ratio
   * @param {number} [targetError]
   * @returns {Mesh}
   */
  simplify(ratio, targetError) {}

  /**
   * @param {number} [iterations]
   * @returns {Mesh}
   */
  subdivideLoop(iterations) {}

  /**
   * @param {number} [iterations]
   * @returns {Mesh}
   */
  subdivideCatmullClark(iterations) {}

  /**
   * @param {number} [lambda]
   * @param {number} [iterations]
   * @returns {Mesh}
   */
  smooth(lambda, iterations) {}

  /**
   * @param {number} [targetEdgeLength]
   * @returns {Mesh}
   */
  remesh(targetEdgeLength) {}

  /**
   * @returns {Mesh}
   */
  repair() {}

  /**
   * @returns {Mesh}
   */
  repairSelfIntersections() {}

  /**
   * @param {Mesh} target
   * @param {number} [factor]
   * @param {number} [offset]
   * @returns {Mesh}
   */
  shrinkwrap(target, factor, offset) {}

  /**
   * @returns {Array<Mesh>}
   */
  splitComponents() {}

  /**
   * @param {Mesh} other
   * @returns {Mesh}
   */
  booleanUnion(other) {}

  /**
   * @param {Mesh} other
   * @returns {Mesh}
   */
  booleanDifference(other) {}

  /**
   * @param {Mesh} other
   * @returns {Mesh}
   */
  booleanIntersection(other) {}

  /**
   * @param {Mesh} other
   * @returns {Mesh}
   */
  csgUnion(other) {}

  /**
   * @param {Mesh} other
   * @returns {Mesh}
   */
  csgSubtract(other) {}

  /**
   * @param {Mesh} other
   * @returns {Mesh}
   */
  csgIntersect(other) {}

  /**
   * @param {string} [method]
   * @returns {Mesh}
   */
  generateUVs(method) {}

  /**
   * @param {string} [projection]
   * @param {Array<number>} [plane]
   * @returns {Mesh}
   */
  projectUVs(projection, plane) {}

  /**
   * @returns {Mesh}
   */
  optimize() {}

  /**
   * @returns {MeshBVH}
   */
  buildBVH() {}

  /**
   * @returns {ProgressiveMesh}
   */
  buildProgressiveMesh() {}

  /**
   * @param {number} [maxVertices]
   * @param {number} [maxTriangles]
   * @returns {MeshletGroupResult}
   */
  buildMeshlets(maxVertices, maxTriangles) {}

  /**
   * @returns {Array<MeshUVDistortionResult>}
   */
  computeUVDistortion() {}

  /**
   * @returns {MeshUVQualityResult}
   */
  measureUVQuality() {}

  /**
   * @returns {Mesh}
   */
  convexHull() {}

  /**
   * @param {MeshConvexDecompParams} [params]
   * @returns {Array<Mesh>}
   */
  convexDecomposition(params) {}

  /**
   * Write the mesh to a file; true when the file was written. A relative
   * path resolves the way `fs.*` resolves it (against the app directory).
   * glTF takes `{skin, skeleton, animations}` to write a rigged asset.
   *
   * @param {string} path
   * @returns {boolean}
   */
  saveOBJ(path) {}

  /**
   * @param {string} path
   * @returns {boolean}
   */
  savePLY(path) {}

  /**
   * @param {string} path
   * @returns {boolean}
   */
  saveSTL(path) {}

  /**
   * @param {string} path
   * @param {Object} [opts]
   * @returns {boolean}
   */
  saveGLTF(path, opts) {}

}

class MeshBVH {

  /**
   * @param {Array<number>} origin
   * @param {Array<number>} direction
   * @param {number} [maxDist]
   * @returns {MeshBVHIntersectResult|null}
   */
  raycast(origin, direction, maxDist) {}

  /**
   * @param {Array<number>} min
   * @param {Array<number>} max
   * @returns {Array<number>}
   */
  queryAABB(min, max) {}

}

class ProgressiveMesh {

  /**
   * @readonly
   * @type {number}
   */
  collapseCount;

  /**
   * @readonly
   * @type {number}
   */
  minVertices;

  /**
   * @readonly
   * @type {number}
   */
  maxVertices;

  /**
   * @param {number} detail
   * @returns {Mesh}
   */
  getMesh(detail) {}

}

/**
 * Half-edge adjacency over N-gon faces: the edit topology a mesh editor
 * keeps beside the triangle Mesh it renders, so a face survives whatever
 * triangulation drew it. Build one from triangles (`fromMeshData` /
 * `fromMesh`, with an optional per-triangle group so coplanar triangles can
 * be merged back into one face with `mergeFacesByGroup`), from a planar
 * polygon, or from N-gon soup; `tessellate()` gives triangles back.
 * Face and vertex indices are stable until `compact()`.
 */
class PolyMesh {

  /**
   *  Empty; the static factories build populated ones.
   */
  constructor() {}

  /**
   * @param {Float32Array} positions
   * @param {Uint32Array} indices
   * @param {Int32Array} [triToGroup]
   * @returns {PolyMesh}
   */
  static fromMeshData(positions, indices, triToGroup) {}

  /**
   * @param {Mesh} mesh
   * @param {Int32Array} [triToGroup]
   * @returns {PolyMesh}
   */
  static fromMesh(mesh, triToGroup) {}

  /**
   *  One N-gon from a simple CCW polygon (as seen from +normal).
   *
   * @param {Float32Array} positionsXYZ
   * @param {Array<number>} normal
   * @param {number} [group]
   * @returns {PolyMesh}
   */
  static fromPolygon(positionsXYZ, normal, group) {}

  /**
   *  N-gon soup: `polyOffsets` (length F+1) delimits each face's run in `polyVerts`.
   *
   * @param {Float32Array} positions
   * @param {Uint32Array} polyVerts
   * @param {Uint32Array} polyOffsets
   * @param {Int32Array} [faceGroups]
   * @returns {PolyMesh}
   */
  static fromPolygons(positions, polyVerts, polyOffsets, faceGroups) {}

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @readonly
   * @type {number}
   */
  halfEdgeCount;

  /**
   * @readonly
   * @type {number}
   */
  faceCount;

  /**
   * @param {number} faceIdx
   * @returns {number}
   */
  faceVertexCount(faceIdx) {}

  /**
   * @param {number} faceIdx
   * @returns {Array<number>}
   */
  faceVertices(faceIdx) {}

  /**
   * @param {number} faceIdx
   * @returns {Array<number>}
   */
  faceHalfEdges(faceIdx) {}

  /**
   * @param {number} vertexIdx
   * @returns {Array<number>}
   */
  getVertex(vertexIdx) {}

  /**
   * @param {number} faceIdx
   * @returns {Array<number>}
   */
  computeFaceNormal(faceIdx) {}

  /**
   *  The face's group tag, or -1.
   *
   * @param {number} faceIdx
   * @returns {number}
   */
  faceGroup(faceIdx) {}

  /**
   * @param {number} faceIdx
   * @param {number} group
   */
  setFaceGroup(faceIdx, group) {}

  /**
   * @param {number} groupId
   * @returns {Array<number>}
   */
  facesInGroup(groupId) {}

  /**
   * @param {number} vertexIdx
   * @returns {boolean}
   */
  isBoundaryVertex(vertexIdx) {}

  /**
   * @param {number} halfEdgeIdx
   * @returns {boolean}
   */
  isBoundaryHalfEdge(halfEdgeIdx) {}

  /**
   *  Outer + hole loops of vertex indices around one face / one group.
   *
   * @param {number} faceIdx
   * @returns {Array<Array<number>>}
   */
  findFaceBoundary(faceIdx) {}

  /**
   * @param {number} groupId
   * @returns {Array<Array<number>>}
   */
  findGroupBoundary(groupId) {}

  /**
   * @returns {MeshTessellation}
   */
  tessellate() {}

  /**
   *  `tessellate()` as a Mesh (positions, flat normals, indices).
   * @returns {Mesh}
   */
  toMesh() {}

  /**
   * @returns {MeshPolyValidation}
   */
  validate() {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {number}
   */
  addVertex(x, y, z) {}

  /**
   *  A face from an ordered vertex loop (3+); call `rematchTwins()` after a batch.
   *
   * @param {Array<number>} vertices
   * @param {number} [group]
   * @returns {number}
   */
  addFace(vertices, group) {}

  /**
   * @param {number} faceIdx
   */
  deleteFace(faceIdx) {}

  /**
   * @param {number} vertexIdx
   * @param {Array<number>} offset
   */
  translateVertex(vertexIdx, offset) {}

  /**
   * @param {number} faceIdx
   * @param {Array<number>} offset
   */
  translateFace(faceIdx, offset) {}

  /**
   *  Push/pull on a closed solid: seam-duplicate vertices move with the face.
   *
   * @param {number} faceIdx
   * @param {Array<number>} offset
   */
  translateFaceWithRing(faceIdx, offset) {}

  /**
   *  SketchUp-style extrusion: the face moves by `offset`, a bridge quad per boundary edge, a back face unless `withBack` is false.
   *
   * @param {number} faceIdx
   * @param {Array<number>} offset
   * @param {boolean} [withBack]
   * @param {number} [bridgeGroup]
   * @param {number} [backGroup]
   * @returns {MeshExtrudeFaceResult}
   */
  extrudeFace(faceIdx, offset, withBack, bridgeGroup, backGroup) {}

  /**
   *  Inset toward the centroid by `amount` (a distance, or a ratio in [0,1) when `asRatio`).
   *
   * @param {number} faceIdx
   * @param {number} amount
   * @param {boolean} [asRatio]
   * @param {number} [bridgeGroup]
   * @returns {MeshInsetFaceResult}
   */
  insetFace(faceIdx, amount, asRatio, bridgeGroup) {}

  /**
   *  Split the edge of half-edge `he` at `position` (default: its midpoint); both faces must be triangles. The new vertex, or -1.
   *
   * @param {number} he
   * @param {Array<number>} [position]
   * @returns {number}
   */
  splitEdge(he, position) {}

  /**
   * @param {number} he
   * @returns {boolean}
   */
  flipEdge(he) {}

  /**
   * @param {number} he
   * @param {Array<number>} [position]
   * @returns {boolean}
   */
  collapseEdge(he, position) {}

  rematchTwins() {}

  mergeFacesByGroup() {}

  compact() {}

}

/**
 * Capsule + sphere occupancy field: the shared obstacle substrate for
 * spaceColonize, placeLeavesOnBranches, scatterLeaves and packAnchors.
 * Queries take a point as [x,y,z] or {x,y,z}; `excludeTag` skips obstacles
 * carrying that tag (a leaf's own branch).
 */
class CapsuleField {

  /**
   * @param {Array<MeshCapsule>} [capsules]
   * @param {Array<MeshSphere>} [spheres]
   * @param {number} [cellSize]
   */
  constructor(capsules, spheres, cellSize) {}

  /**
   * @readonly
   * @type {boolean}
   */
  empty;

  /**
   * @readonly
   * @type {number}
   */
  capsuleCount;

  /**
   * @readonly
   * @type {number}
   */
  sphereCount;

  /**
   * @readonly
   * @type {number}
   */
  cellSize;

  /**
   * @param {Array<number>} point
   * @param {number} [excludeTag]
   * @param {number} [extraClearance]
   * @returns {boolean}
   */
  contains(point, excludeTag, extraClearance) {}

  /**
   * @param {Array<number>} point
   * @param {number} clearance
   * @param {number} [excludeTag]
   * @returns {boolean}
   */
  tooClose(point, clearance, excludeTag) {}

  /**
   *  Signed distance to the nearest obstacle surface (negative inside).
   *
   * @param {Array<number>} point
   * @param {number} [excludeTag]
   * @returns {number}
   */
  distance(point, excludeTag) {}

  /**
   * @param {Array<number>} point
   * @param {number} [excludeTag]
   * @returns {MeshCapsuleFieldNearest|null}
   */
  nearest(point, excludeTag) {}

  /**
   * @param {Array<number>} center
   * @param {number} radius
   * @param {number} [excludeTag]
   * @returns {boolean}
   */
  intersectsSphere(center, radius, excludeTag) {}

}

class LSystem {

  /**
   * @param {string} [axiom]
   */
  constructor(axiom) {}

  /**
   * @param {string} text
   * @returns {LSystem}
   */
  setAxiom(text) {}

  /**
   * @param {string} predecessor
   * @param {string} successor
   * @param {number} [weight]
   * @returns {LSystem}
   */
  addRule(predecessor, successor, weight) {}

  /**
   * @param {number} iterations
   * @param {number} [seed]
   * @returns {string}
   */
  derive(iterations, seed) {}

  /**
   * @param {number} iterations
   * @param {number} [seed]
   * @returns {Array<MeshLSystemModule>}
   */
  deriveModules(iterations, seed) {}

}

