// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Mesh API Reference
 * =============================================================================
 *
 * The bromesh C++ library is exposed via these classes and namespaces:
 * Mesh, MeshBVH, ProgressiveMesh, PolyMesh, LSystem.
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
 * @typedef {Object} MeshBranchSegment
 * @property {Array<number>} [p0]
 * @property {Array<number>} [p1]
 * @property {number} [r0]
 * @property {number} [r1]
 * @property {Array<number>} [dir]
 * @property {number} [parent]
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
 * @property {Uint32Array} [dupVerts]
 * @property {Uint32Array} [bridgeFaces]
 * @property {Uint32Array} [bridgeAdjGroup]
 * @property {number} [backFace]
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
   * @param {Array<number>} points
   * @param {number} [radius]
   * @param {number} [segments]
   * @returns {Mesh}
   */
  static tube(points, radius, segments) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {Mesh}
   */
  static fromGLTF(buffer) {}

  /**
   * @param {string} text
   * @returns {Mesh}
   */
  static fromOBJ(text) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {Mesh}
   */
  static fromPLY(buffer) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {Mesh}
   */
  static fromSTL(buffer) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {Mesh}
   */
  static fromFBX(buffer) {}

  /**
   * @param {ArrayBuffer} buffer
   * @returns {Mesh}
   */
  static fromVOX(buffer) {}

  /**
   * @param {Array<Mesh>} meshes
   * @returns {Mesh}
   */
  static merge(meshes) {}

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
   * @param {Array<number>} path
   * @param {Array<number>} profile
   * @returns {Mesh}
   */
  static sweep(path, profile) {}

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
   * @param {MeshTreeOptions} [opts]
   * @returns {Object}
   */
  static tree(opts) {}

  /**
   * @param {string} text
   * @returns {Array<MeshLSystemModule>}
   */
  static parseLSystem(text) {}

  /**
   * @param {Array<Array<number>>} candidates
   * @param {Object} [opts]
   * @returns {Int32Array}
   */
  static packAnchors(candidates, opts) {}

  /**
   * @param {Array<MeshLSystemModule>} modules
   * @param {Object} [opts]
   * @returns {Array<MeshBranchSegment>}
   */
  static lsystemToBranches(modules, opts) {}

  /**
   * @param {Array<Object>} capsules
   * @param {Array<Object>} [spheres]
   * @param {number} [cellSize]
   * @returns {Object}
   */
  static capsuleField(capsules, spheres, cellSize) {}

  /**
   * @param {Array<MeshBranchSegment>} segments
   * @param {number} [radiusScale]
   * @param {Array<Object>} [extraSpheres]
   * @returns {Object}
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
   * @param {string} [name]
   * @returns {Uint8Array}
   */
  toGLTF(name) {}

  /**
   * @returns {string}
   */
  toOBJ() {}

  /**
   * @returns {string}
   */
  toPLY() {}

  /**
   * @returns {string}
   */
  toSTL() {}

  /**
   * @returns {ArrayBuffer}
   */
  toSTLB() {}

  /**
   * @returns {ArrayBuffer}
   */
  toFBX() {}

  /**
   * @returns {ArrayBuffer}
   */
  toVOX() {}

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

class PolyMesh {

  /**
   * @param {Mesh} [mesh]
   */
  constructor(mesh) {}

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @readonly
   * @type {number}
   */
  faceCount;

  /**
   * @readonly
   * @type {number}
   */
  edgeCount;

  /**
   * @returns {Mesh}
   */
  toMesh() {}

  /**
   * @param {number} faceIdx
   * @param {Array<number>} offset
   * @param {boolean} [withBack]
   * @param {number} [bridgeGroup]
   * @param {number} [backGroup]
   * @returns {MeshExtrudeFaceResult}
   */
  extrudeFace(faceIdx, offset, withBack, bridgeGroup, backGroup) {}

  rematchTwins() {}

  mergeFacesByGroup() {}

  compact() {}

  /**
   * @param {number} groupId
   * @returns {Array<Array<number>>}
   */
  findGroupBoundary(groupId) {}

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

