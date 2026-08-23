// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 * =============================================================================
 * bro Rigging, Animation & Inverse Kinematics API Reference
 * =============================================================================
 *
 * Skeletal structures, joint hierarchies, inverse kinematics solvers (TwoBoneIK, FABRIK, LookAt),
 * skinning weight assignment, automated procedural rigging, and pose retargeting.
 * @typedef {Object} SkinDataOptions
 * @property {number} [vertexCount]
 * @property {number} [maxWeightsPerVertex]
 * @property {Uint16Array} [indices]
 * @property {Float32Array} [weights]
 */

/**
 * @typedef {Object} SkeletonBone
 * @property {string} [name]
 * @property {number} [parent]
 * @property {Array<number>} [localBindPose]
 * @property {Array<number>} [inverseBindMatrix]
 */

/**
 * @typedef {Object} SkeletonSocket
 * @property {string} [name]
 * @property {number} [boneIndex]
 * @property {Array<number>} [offset]
 */

/**
 * @typedef {Object} SkeletonOptions
 * @property {Array<SkeletonBone>} [bones]
 * @property {Array<SkeletonSocket>} [sockets]
 */

/**
 * @typedef {Object} PoseTRS
 * @property {Array<number>} [translation]
 * @property {Array<number>} [rotation]
 * @property {Array<number>} [scale]
 */

/**
 * @typedef {Object} IKTwoBoneOptions
 * @property {Array<number>} [rootPos]
 * @property {Array<number>} [midPos]
 * @property {Array<number>} [endPos]
 * @property {Array<number>} [targetPos]
 * @property {Array<number>} [poleVector]
 * @property {number} [length1]
 * @property {number} [length2]
 */

/**
 * @typedef {Object} IKFabrikOptions
 * @property {Array<Array<number>>} [jointPositions]
 * @property {Array<number>} [targetPos]
 * @property {number} [tolerance]
 * @property {number} [maxIterations]
 */

/**
 * @typedef {Object} IKLookAtOptions
 * @property {Array<number>} [headPos]
 * @property {Array<number>} [targetPos]
 * @property {Array<number>} [forward]
 * @property {Array<number>} [up]
 * @property {number} [maxAngle]
 */

/**
 * @typedef {Object} RigLandmark
 * @property {string} [name]
 * @property {Array<number>} [position]
 * @property {number} [confidence]
 */

/**
 * @typedef {Object} RigDetectionResult
 * @property {Array<RigLandmark>} [landmarks]
 * @property {string} [detectedType]
 */

/**
 * @typedef {Object} RigFitResult
 * @property {Skeleton} [skeleton]
 * @property {SkinData} [skin]
 */

/**
 * @typedef {Object} AutoRigOptions
 * @property {string} [rigType]
 * @property {number} [maxBones]
 * @property {boolean} [voxelize]
 * @property {number} [voxelResolution]
 */

/**
 * @typedef {Object} LocomotionOptions
 * @property {number} [speed]
 * @property {number} [strideLength]
 * @property {number} [cycleDuration]
 */

// ── Classes & Interfaces ─────────────────────────────────────────────────────

class SkinData {

  /**
   * @param {SkinDataOptions} [opts]
   */
  constructor(opts) {}

  /**
   * @readonly
   * @type {number}
   */
  vertexCount;

  /**
   * @readonly
   * @type {number}
   */
  maxWeights;

  /**
   * @returns {SkinData}
   */
  clone() {}

  /**
   * @returns {boolean}
   */
  validate() {}

  normalize() {}

}

class Skeleton {

  /**
   * @param {SkeletonOptions} [opts]
   */
  constructor(opts) {}

  /**
   * @readonly
   * @type {number}
   */
  boneCount;

  /**
   * @param {string} name
   * @returns {number}
   */
  findBone(name) {}

  /**
   * @param {number} index
   * @returns {string}
   */
  boneName(index) {}

  /**
   * @param {number} index
   * @returns {number}
   */
  boneParent(index) {}

  /**
   * @param {number} index
   * @returns {Array<number>}
   */
  boneBindPose(index) {}

  /**
   * @param {number} index
   * @returns {Array<number>}
   */
  boneInverseBind(index) {}

  /**
   * @returns {Skeleton}
   */
  clone() {}

}

class Pose {

  /**
   * @param {Skeleton} skeleton
   */
  constructor(skeleton) {}

  /**
   * @readonly
   * @type {number}
   */
  boneCount;

  /**
   * @param {number} index
   * @returns {PoseTRS}
   */
  getBoneLocal(index) {}

  /**
   * @param {number} index
   * @param {PoseTRS} trs
   */
  setBoneLocal(index, trs) {}

  /**
   * @param {number} index
   * @returns {Array<number>}
   */
  getBoneModelMatrix(index) {}

  /**
   * @returns {Pose}
   */
  clone() {}

}

class SkeletalAnimation {

  /**
   * @readonly
   * @type {string}
   */
  name;

  /**
   * @readonly
   * @type {number}
   */
  duration;

  /**
   * @readonly
   * @type {number}
   */
  trackCount;

  /**
   * @param {number} time
   * @param {Pose} outPose
   */
  evaluate(time, outPose) {}

}

class RigSpec {

  /**
   * @param {string} type
   */
  constructor(type) {}

  /**
   * @readonly
   * @type {string}
   */
  type;

  /**
   * @readonly
   * @type {Array<string>}
   */
  requiredBones;

}

class VoxelChunk {

  /**
   * @param {number} dimX
   * @param {number} dimY
   * @param {number} dimZ
   */
  constructor(dimX, dimY, dimZ) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @param {number} value
   */
  set(x, y, z, value) {}

  /**
   * @param {number} x
   * @param {number} y
   * @param {number} z
   * @returns {number}
   */
  get(x, y, z) {}

  /**
   * @returns {Mesh}
   */
  toMesh() {}

}

class IK {

  /**
   * @param {IKTwoBoneOptions} opts
   * @returns {Array<Array<number>>}
   */
  static solveTwoBone(opts) {}

  /**
   * @param {IKFabrikOptions} opts
   * @returns {Array<Array<number>>}
   */
  static solveFabrik(opts) {}

  /**
   * @param {IKLookAtOptions} opts
   * @returns {Array<number>}
   */
  static solveLookAt(opts) {}

}

class Rig {

  /**
   * @param {Mesh} mesh
   * @returns {RigDetectionResult}
   */
  static detectLandmarks(mesh) {}

  /**
   * @param {Mesh} mesh
   * @param {RigSpec} spec
   * @returns {RigFitResult}
   */
  static fitSkeleton(mesh, spec) {}

  /**
   * @param {Mesh} mesh
   * @param {AutoRigOptions} [opts]
   * @returns {RigFitResult}
   */
  static autoRig(mesh, opts) {}

  /**
   * @param {Mesh} sourceMesh
   * @param {SkinData} sourceSkin
   * @param {Mesh} targetMesh
   * @returns {SkinData}
   */
  static transferWeights(sourceMesh, sourceSkin, targetMesh) {}

}

