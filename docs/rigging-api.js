// ── bro.rigging ──────────────────────────────────────────────────────────────
//
// Skinning and skeletal animation: `SkinData`, `Skeleton` / `Joint`, `Pose`,
// `AnimationClip`, the `IK` solvers, the `Rig` auto-rigging pipeline, and the
// rigged-glTF entry points bromesh installs onto `Mesh`.
//
// Geometry lives beside this file: docs/mesh-api.js (the `Mesh` class),
// docs/mesh-io-api.js (files, isosurfaces, `PolyMesh`, `VoxelChunk`),
// docs/mesh-plants-api.js (sweeps, branch trees, L-systems).
//
// Mount points. `bro.rigging` carries `SkinData`, `Skeleton`, `Joint`,
// `SkeletonRig` (aliased `RigSpec` and `Rig`), `Pose`, `AnimationClip`
// (aliased `Animation` and `SkeletalAnimation`), `VoxelChunk` and `IK`, plus
// the forwarded rig statics `specFromFile`, `detectHumanoid`,
// `detectLandmarks`, `detectQuadruped`, `missingLandmarks`, `fitSkeleton`,
// `autoRig` and `transferWeights`. Every one of those classes is also a real
// global, and the statics NOT forwarded onto `bro.rigging` — `Rig.spec`,
// `Rig.specFromJSON`, `Rig.generateLocomotionCycle` — are reachable on the
// global `Rig` / `RigSpec` / `SkeletonRig` constructor. `VoxelChunk` is
// mounted here too but has nothing to do with rigging — it is documented with
// the other voxel entry points in docs/mesh-io-api.js.
//
// Conventions
//   Quaternions are xyzw.
//   Matrices are column-major 4x4 (16 floats), matching glTF.
//   Pose data is one flat Float32Array, stride 10 per bone: T (3), R (4 xyzw), S (3).
//   Bone indices in a skin or a clip refer to `skeleton.bones` order.
//
// A typical flow:
//   const mesh = Mesh.loadGLTF('char.glb').meshes[0];
//   const r = Rig.autoRig(mesh, { rigType: 'humanoid' });
//   const clip = Rig.generateLocomotionCycle(r.skeleton, Rig.spec('humanoid'), 'walk');
//
//   // per frame
//   const pose = clip.evaluate(r.skeleton, t);
//   mesh.applySkinning(r.skin, pose.computeSkinningMatrices(r.skeleton));

// ── Dictionaries ─────────────────────────────────────────────────────────────

/**
 *  `SkinData.validate` / `skin.validate()`: how healthy the weights are.
 * @typedef {Object} SkinValidation
 * @property {boolean} [clean] -  true when nothing below is out of range.
 * @property {number} [vertexCount]
 * @property {number} [orphanCount] -  vertices with no usable influence.
 * @property {number} [badSumCount] -  vertices whose weights do not sum to 1.
 * @property {number} [nanCount]
 * @property {number} [maxSumDeviation]
 * @property {number} [maxInfluencesObserved]
 */

/**
 *  A named attachment point on a bone.
 * @typedef {Object} Socket
 * @property {string} [name]
 * @property {number} [bone] -  bone index; `boneIndex` is the same value.
 * @property {number} [boneIndex]
 * @property {Float32Array} [offset] -  column-major 4x4 relative to the bone.
 */

/**
 *  One animation channel. `path` is 'translation' | 'rotation' | 'scale';
 *  `interp` is 'linear' | 'step' | 'cubicspline'. `values` holds 3, 4 or 3
 *  floats per key respectively.
 * @typedef {Object} AnimChannel
 * @property {number} [boneIndex]
 * @property {string} [path]
 * @property {string} [interp]
 * @property {Float32Array} [times] -  seconds.
 * @property {Float32Array} [values]
 */

/**
 *  Detected landmarks: a wrapper around a name → [x,y,z] map. The same shape
 *  is accepted back by `missingLandmarks`, `fitSkeleton` and `autoRig`, which
 *  also take a bare `{ name: [x,y,z] }` object.
 * @typedef {Object} Landmarks
 * @property {Object} [points]
 */

/**
 *  What `Rig.autoRig` returns.
 * @typedef {Object} AutoRigResult
 * @property {Skeleton} [skeleton]
 * @property {SkinData} [skin]
 * @property {string} [methodUsed] -  'bbw' | 'boneHeat' | 'voxelBind'.
 * @property {Array<string>} [missingLandmarks]
 * @property {Array<string>} [warnings]
 */

/**
 *  Everything `Mesh.loadGLTF` reads out of a file. `meshSkeleton[i]` and
 *  `animationSkeleton[i]` index `skeletons`.
 * @typedef {Object} MeshGltfScene
 * @property {Array<Mesh>} [meshes]
 * @property {Array<SkinData>} [skins]
 * @property {Array<Skeleton>} [skeletons]
 * @property {Array<AnimationClip>} [animations]
 * @property {Array<number>} [meshSkeleton]
 * @property {Array<number>} [animationSkeleton]
 */

/**
 *  A morph target, as `mesh.applyMorphTarget` takes it.
 * @typedef {Object} MeshMorphTarget
 * @property {string} [name]
 * @property {Float32Array} [deltaPositions] -  xyz per vertex.
 * @property {Float32Array} [deltaNormals]
 * @property {number} [weight] -  used when no explicit weight argument is passed.
 */

// ── SkinData ─────────────────────────────────────────────────────────────────

/**
 * Per-vertex bone influences plus the inverse-bind matrices. Exactly four
 * influences per vertex, always — `maxWeights` is a constant 4.
 */
class SkinData {

  /**
   * @param {Object} [opts]
   * @param {Float32Array} [opts.boneWeights] -  4 per vertex; `weights` is accepted as a synonym.
   * @param {Uint32Array} [opts.boneIndices] -  4 per vertex; `indices` is accepted as a synonym.
   * @param {Float32Array} [opts.inverseBindMatrices] -  column-major mat4 per bone, stride 16.
   * @param {number} [opts.boneCount] -  else derived from `inverseBindMatrices`.
   */
  constructor(opts) {}

  /** @readonly @type {Float32Array} `weights` is an alias. */
  boneWeights;

  /** @readonly @type {Uint32Array} `indices` is an alias. */
  boneIndices;

  /** @readonly @type {Float32Array} */
  inverseBindMatrices;

  /** @readonly @type {number} */
  boneCount;

  /** @readonly @type {number} */
  vertexCount;

  /** @readonly @type {number} always 4. */
  maxWeights;

  /** Scale each vertex's four weights to sum to 1. @returns {SkinData} this */
  normalize() {}

  /** @returns {SkinData} a deep copy */
  clone() {}

  /**
   *  Validate against a mesh; without one, a stand-in of the right vertex
   *  count is used, so the geometry-independent checks still run.
   * @param {Mesh} [mesh]
   * @returns {SkinValidation}
   */
  validate(mesh) {}

  /**
   * @param {Mesh} mesh @param {SkinData} skin
   * @returns {SkinValidation}
   */
  static validate(mesh, skin) {}

  /**
   * Project weights from `sourceMesh` / `sourceSkin` onto `targetMesh` by
   * closest-point interpolation — how you re-rig a retopologised or swapped
   * garment against a skeleton that already works. The same Skeleton applies
   * to the result. `Rig.transferWeights` is the same function.
   *
   * @param {Mesh} targetMesh
   * @param {Mesh} sourceMesh
   * @param {SkinData} sourceSkin
   * @param {number} [maxDistance=0] -  0 = unlimited.
   * @returns {SkinData}
   */
  static transfer(targetMesh, sourceMesh, sourceSkin, maxDistance) {}

}

// ── Joint ────────────────────────────────────────────────────────────────────

/**
 * One bone. `skeleton.bones` hands these out; each is a COPY of the skeleton's
 * bone, so mutating one does not edit the skeleton — rebuild the Skeleton from
 * a bone array if you need to change the hierarchy.
 */
class Joint {

  /**
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.parent] -  index, -1 at a root.
   * @param {number} [opts.index]
   * @param {Array<number>} [opts.localT] -  `translation` is accepted as a synonym.
   * @param {Array<number>} [opts.localR] -  xyzw; `rotation` is a synonym.
   * @param {Array<number>} [opts.localS] -  `scale` is a synonym.
   * @param {Array<number>} [opts.inverseBind] -  mat4; `inverseBindMatrix` is a synonym.
   */
  constructor(opts) {}

  /** @type {string} */
  name;

  /** @type {number} parent bone index, -1 at a root. */
  parent;

  /** @type {number} */
  index;

  /** @readonly @type {Float32Array} */
  localT;

  /** @readonly @type {Float32Array} xyzw. */
  localR;

  /** @readonly @type {Float32Array} */
  localS;

  /** @readonly @type {Float32Array} column-major mat4. */
  inverseBind;

}

// ── Skeleton ─────────────────────────────────────────────────────────────────

/**
 * A bone hierarchy plus its named sockets. Bones must be stored parent-before-
 * child; `parent === -1` marks a root.
 */
class Skeleton {

  /**
   * @param {Object} [opts]
   * @param {Array<Joint|Object>} [opts.bones] -  `Joint` instances, or plain `{name, parent, localT, localR, localS, inverseBind}`.
   * @param {Array<Socket>} [opts.sockets]
   */
  constructor(opts) {}

  /** @param {Array<Joint|Object>} bones @returns {Skeleton} */
  static fromBones(bones) {}

  /** @readonly @type {Array<Joint>} a fresh Joint per bone on every read. */
  bones;

  /** @readonly @type {Array<Socket>} */
  sockets;

  /** @readonly @type {number} */
  boneCount;

  /** @readonly @type {number} */
  socketCount;

  /** @param {string} name @returns {number} bone index, or -1 */
  findBone(name) {}

  /**
   *  The first bone whose name ENDS with `suffix` — how you find "…:Hand_L"
   *  across Mixamo's and Rigify's different prefixes.
   * @param {string} suffix
   * @returns {number} bone index, or -1
   */
  findBoneBySuffix(suffix) {}

  /** @param {string} name @returns {number} socket index, or -1 */
  findSocket(name) {}

  /**
   *  Append a socket. Takes a `Socket` object, or the loose
   *  `(name, boneIndex, offsetMat4)` form.
   * @param {Socket|string} socket
   * @param {number} [boneIndex=0]
   * @param {Array<number>} [offset]
   * @returns {number} the new socket index
   */
  addSocket(socket, boneIndex, offset) {}

  /**
   * Append the standard attachment sockets — hands, feet, head, spine — for a
   * Rigify- or Mixamo-named skeleton, matching the `ORG-`, `DEF-`, bare and
   * `mixamorig:` bone spellings. Returns how many were added, so 0 means none
   * of the expected bone names were found.
   *
   * @returns {number}
   *
   * @example
   *   const { skeletons } = Mesh.loadGLTF('char.glb');
   *   const skel = skeletons[0];
   *   if (skel.addRigifySockets() > 0) {
   *     const m = pose.socketWorld(skel, 'hand_R');   // mat4 or null
   *   }
   */
  addRigifySockets() {}

  /** The identity / bind pose for this skeleton. @returns {Pose} */
  bindPose() {}

  /** @returns {Skeleton} a deep copy */
  clone() {}

}

// ── Pose ─────────────────────────────────────────────────────────────────────

/**
 * Local TRS per bone in one flat Float32Array, stride 10: T (3), R (4 xyzw),
 * S (3). Cheap to blend, cheap to keep around; turn it into matrices only when
 * you are about to skin with it.
 */
class Pose {

  /**
   * @param {Skeleton|number|Float32Array} [init] -  a Skeleton gives its bind pose; a number gives that many identity bones; a Float32Array is adopted as the raw data.
   * @param {number} [boneCount] -  with a Float32Array, pads it out to this many bones.
   */
  constructor(init, boneCount) {}

  /** @type {Float32Array} stride 10 per bone; writable. */
  data;

  /** @readonly @type {number} */
  boneCount;

  /**
   *  World transforms, 16 floats per bone.
   * @param {Skeleton} skeleton
   * @returns {Float32Array}
   */
  computeWorldMatrices(skeleton) {}

  /**
   *  World x inverseBind per bone — what `mesh.applySkinning` wants.
   * @param {Skeleton} skeleton
   * @returns {Float32Array}
   */
  computeSkinningMatrices(skeleton) {}

  /**
   *  The world mat4 of a named socket, or null when the skeleton has no such
   *  socket.
   * @param {Skeleton} skeleton @param {string} socketName
   * @returns {Float32Array|null}
   */
  socketWorld(skeleton, socketName) {}

  /** @returns {Pose} a deep copy */
  clone() {}

  /**
   *  Blend `b` into `a` IN PLACE — lerp T and S, slerp R — and return `a`.
   *  `mask` is a Uint8Array of length boneCount; 1 enables the blend on that
   *  bone.
   * @param {Pose} a @param {Pose} b @param {number} weight @param {Uint8Array} [mask]
   * @returns {Pose} `a`
   */
  static blend(a, b, weight, mask) {}

  /**
   * Weighted N-way blend into a NEW Pose. Weights are normalized internally;
   * translations and scales are weighted sums, rotations a weighted nlerp
   * hemisphere-aligned against the highest-weight pose. With exactly two poses
   * it takes the same path as `blend`. Masked-out bones take the
   * highest-weight pose's values. `weights.length` must equal `poses.length`.
   *
   * @param {Array<Pose>} poses
   * @param {Array<number>|Float32Array} weights
   * @param {Uint8Array} [mask]
   * @returns {Pose}
   */
  static blendN(poses, weights, mask) {}

}

// ── AnimationClip ────────────────────────────────────────────────────────────

/**
 * Keyframed channels over a skeleton's bones. `Animation` and
 * `SkeletalAnimation` are aliases of the same class.
 */
class AnimationClip {

  /**
   * @param {Object} [opts]
   * @param {string} [opts.name]
   * @param {number} [opts.duration] -  seconds.
   * @param {Array<AnimChannel>} [opts.channels]
   */
  constructor(opts) {}

  /** @type {string} */
  name;

  /** @type {number} seconds. */
  duration;

  /** @readonly @type {Array<AnimChannel>} */
  channels;

  /** @readonly @type {number} */
  channelCount;

  /**
   *  Sample at `t` seconds into a NEW Pose. `loop` may be a boolean or
   *  `{ loop }`, and defaults to true.
   * @param {Skeleton} skeleton @param {number} [t=0] @param {boolean|Object} [loop=true]
   * @returns {Pose}
   */
  evaluate(skeleton, t, loop) {}

  /**
   *  The allocation-free form: sample into an existing Pose and return it.
   *  Both `(skeleton, t, pose, loop?)` and `(skeleton, t, loop, pose)` are
   *  accepted.
   * @param {Skeleton} skeleton @param {number} t @param {Pose} pose @param {boolean} [loop=true]
   * @returns {Pose} the pose passed in
   */
  evaluateInto(skeleton, t, pose, loop) {}

  /**
   *  Remap a clip from one skeleton to another by bone NAME; bones the target
   *  does not have are dropped.
   * @param {AnimationClip} anim @param {Skeleton} srcSkeleton @param {Skeleton} dstSkeleton
   * @returns {AnimationClip}
   */
  static retarget(anim, srcSkeleton, dstSkeleton) {}

}

// ── IK ───────────────────────────────────────────────────────────────────────
//
// `bro.rigging.IK`, also the global `IK`. Every solver MUTATES the supplied
// Pose and returns a boolean for whether it converged. Targets are world-space
// [x,y,z].

/**
 *  Analytic two-bone IK over `root` → `mid` → `end`. `pole` steers the elbow
 *  or knee plane; omit it to keep the current plane.
 * @param {Skeleton} skel @param {Pose} pose @param {number} root @param {number} mid @param {number} end
 * @param {Array<number>} target @param {Array<number>} [pole]
 * @returns {boolean}
 *
 * @example
 *   IK.twoBone(skel, pose, shoulder, elbow, wrist, handTarget, [0, 0, -1]);
 */
IK.twoBone = function(skel, pose, root, mid, end, target, pole) {};

/**
 *  FABRIK over an arbitrary chain of bone indices, root first.
 * @param {Skeleton} skel @param {Pose} pose @param {Array<number>} chain @param {Array<number>} target
 * @param {Object} [opts]
 * @param {number} [opts.iterations=10]
 * @param {number} [opts.tolerance=1e-3]
 * @returns {boolean}
 */
IK.FABRIK = function(skel, pose, chain, target, opts) {};

/**
 *  Orient a single bone at a target.
 * @param {Skeleton} skel @param {Pose} pose @param {number} bone @param {Array<number>} target
 * @param {Object} [opts]
 * @param {Array<number>} [opts.forward=[0,0,1]] -  the bone's local forward axis.
 * @param {Array<number>} [opts.up=[0,1,0]]
 * @returns {boolean}
 */
IK.lookAt = function(skel, pose, bone, target, opts) {};

/**
 *  Options-object wrapper over `twoBone`:
 *  `{ skel, pose, root, mid, end, targetPos, poleVector }`.
 * @param {Object} opts @returns {boolean}
 */
IK.solveTwoBone = function(opts) {};

/**
 *  Options-object wrapper over `FABRIK`:
 *  `{ skel, pose, chain, targetPos, maxIterations, tolerance }`.
 * @param {Object} opts @returns {boolean}
 */
IK.solveFabrik = function(opts) {};

/**
 *  Options-object wrapper over `lookAt`:
 *  `{ skel, pose, bone, targetPos, forward, up }`.
 * @param {Object} opts @returns {boolean}
 */
IK.solveLookAt = function(opts) {};

// ── SkeletonRig / Rig ────────────────────────────────────────────────────────

/**
 * A rig template: the bones a skeleton should have and the landmarks needed to
 * place them. `Rig`, `RigSpec` and `SkeletonRig` are the same constructor, and
 * the auto-rigging pipeline hangs off it as statics.
 */
class SkeletonRig {

  /**
   *  `'humanoid'` or `'quadruped'` builds the matching built-in; any other
   *  string names an empty spec. An object form reads `type` or `name`. No
   *  argument gives the humanoid spec.
   * @param {string|Object} [type='humanoid']
   */
  constructor(type) {}

  /** @readonly @type {string} `type` and `name` are the same value. */
  type;

  /** @readonly @type {string} */
  name;

  /** @readonly @type {number} */
  boneCount;

  /** @readonly @type {number} */
  landmarkCount;

  /** @readonly @type {number} attachment sockets the spec declares */
  socketCount;

  /** @readonly @type {boolean} whether fitting enforces left/right mirror symmetry */
  symmetric;

  /** @returns {string} the spec as JSON text */
  toJSON() {}

  /** @returns {Array<string>} */
  landmarkNames() {}

  /** @returns {Array<string>} */
  boneNames() {}

}

/**
 *  A built-in spec by name — 'humanoid' or 'quadruped'. Global `Rig` only; not
 *  forwarded onto `bro.rigging`.
 * @param {string} name
 * @returns {SkeletonRig}
 */
Rig.spec = function(name) {};

/** Global `Rig` only. @param {string} jsonText @returns {SkeletonRig} */
Rig.specFromJSON = function(jsonText) {};

/**
 *  Load a spec from a JSON file. Note this path does NOT go through the app
 *  path resolver the mesh loaders use.
 * @param {string} path
 * @returns {SkeletonRig}
 */
Rig.specFromFile = function(path) {};

/**
 *  Geometric landmark detection on a humanoid mesh. `Rig.detectLandmarks` is
 *  the same function.
 * @param {Mesh} mesh
 * @returns {Landmarks}
 */
Rig.detectHumanoid = function(mesh) {};

/** @param {Mesh} mesh @returns {Landmarks} */
Rig.detectLandmarks = function(mesh) {};

/** @param {Mesh} mesh @returns {Landmarks} */
Rig.detectQuadruped = function(mesh) {};

/**
 *  Which of the spec's landmarks the supplied set is missing — check this
 *  before `fitSkeleton` rather than debugging a bent skeleton afterwards.
 * @param {SkeletonRig} spec @param {Landmarks|Object} landmarks
 * @returns {Array<string>}
 */
Rig.missingLandmarks = function(spec, landmarks) {};

/**
 *  Build a Skeleton positioned against `mesh` from a spec and its landmarks.
 * @param {SkeletonRig} spec @param {Landmarks|Object} landmarks @param {Mesh} mesh
 * @returns {Skeleton}
 */
Rig.fitSkeleton = function(spec, landmarks, mesh) {};

/**
 * Detect, fit and weight in one call. Two argument shapes:
 * `(mesh, spec, landmarks, opts?)`, or `(mesh, opts?)` with the spec and
 * landmarks inside the options. Landmarks are detected automatically when not
 * supplied — quadruped detection when the spec is named 'quadruped',
 * humanoid otherwise.
 *
 * Each per-method block (`voxel`, `boneHeat`, `bbw`) is read independently,
 * so a caller can set all three and switch `method`; only the block matching
 * the method used is consulted.
 *
 * @param {Mesh} mesh
 * @param {SkeletonRig|Object} [specOrOpts]
 * @param {Landmarks|Object} [landmarks]
 * @param {Object} [opts]
 * @param {SkeletonRig} [opts.spec]
 * @param {string} [opts.rigType] -  used when no `spec` is given.
 * @param {Landmarks|Object} [opts.landmarks]
 * @param {string} [opts.method] -  'auto' | 'voxelBind' | 'boneHeat' | 'bbw'.
 * @param {number} [opts.smoothIterations=2] -  Laplacian smoothing passes on the final skin (all methods).
 * @param {number} [opts.smoothAlpha=0.5]
 * @param {number} [opts.minWeight=0.001] -  top-K pruner threshold.
 * @param {{maxResolution?: number, maxInfluences?: number, falloffPower?: number, minWeight?: number, smoothIterations?: number, smoothAlpha?: number}} [opts.voxel]
 *   voxelBind: grid resolution (96), influences per vertex (4), falloff exponent (4), its own smoothing.
 * @param {{maxInfluences?: number, minWeight?: number, heatStrength?: number, solverTol?: number, solverMaxIter?: number}} [opts.boneHeat]
 *   boneHeat: influences (4), heat-source strength (1.0), CG tolerance (1e-7) / iteration cap (2000).
 * @param {{maxInfluences?: number, minWeight?: number, anchorsPerBone?: number, eps?: number, maxIter?: number}} [opts.bbw]
 *   bbw: influences (4), anchors per bone (3), OSQP tolerance (1e-4) / iteration cap (5000).
 * @returns {AutoRigResult}
 *
 * @example
 *   const r = Rig.autoRig(mesh, { rigType: 'humanoid', method: 'bbw' });
 *   if (r.warnings.length) console.warn(r.warnings.join('\n'));
 *   mesh.applySkinning(r.skin, r.skeleton.bindPose().computeSkinningMatrices(r.skeleton));
 */
Rig.autoRig = function(mesh, specOrOpts, landmarks, opts) {};

/**
 *  A synthesized walk / run cycle for a fitted skeleton. `params` is a gait
 *  NAME string, or the parameter object below. Global `Rig` only; not
 *  forwarded onto `bro.rigging`.
 * @param {Skeleton} skeleton @param {SkeletonRig} spec
 * @param {string|Object} [params]
 * @param {number} [params.strideLength=0.3] -  world units per stride.
 * @param {number} [params.cycleDuration=1.0] -  seconds; the clip's duration.
 * @param {number} [params.footLiftHeight=0.08]
 * @param {number} [params.keyframesPerCycle=24]
 * @param {number} [params.bodyBobAmplitude=0.02] -  0 disables the root bob.
 * @param {number} [params.armSwingAmplitude=0.35] -  radians; 0 disables.
 * @param {Array<number>} [params.forwardAxis=[0,0,1]]
 * @param {Array<number>} [params.upAxis=[0,1,0]]
 * @param {string|{name?: string, phases?: Array<number>, dutyFactor?: number}} [params.gait]
 *   gait name, or per-leg phase offsets in [0,1) and the stance fraction (0.6).
 * @returns {AnimationClip}
 */
Rig.generateLocomotionCycle = function(skeleton, spec, params) {};

/** Alias of `SkinData.transfer`. @returns {SkinData} */
Rig.transferWeights = function(targetMesh, sourceMesh, sourceSkin, maxDistance) {};

// ── Mesh extensions ──────────────────────────────────────────────────────────
//
// bromesh's rigging half adds these to the `Mesh` class documented in
// docs/mesh-api.js. `Mesh.loadGLTF` and `mesh.saveGLTF` exist only when
// bromesh was built with glTF support; `Mesh.loadGLTF` is also forwarded onto
// `bro.mesh`.

/**
 *  Deform the mesh in place by `skin` and a flat array of column-major 4x4
 *  skinning matrices — exactly what `pose.computeSkinningMatrices()` returns.
 *  Fewer matrices than the skin's bone count throws.
 * @param {SkinData} skin @param {Float32Array} matrices
 * @returns {Mesh} this
 */
Mesh.prototype.applySkinning = function(skin, matrices) {};

/**
 *  Add `weight` times a morph target's deltas. Takes a target object, or the
 *  loose `(name, deltaPositions, weight)` / `(name, deltaPositions,
 *  deltaNormals, weight)` form.
 * @param {MeshMorphTarget|string} target @param {number} [weight=1]
 * @returns {Mesh} this
 */
Mesh.prototype.applyMorphTarget = function(target, weight) {};

/**
 * Read a whole glTF / glb scene: geometry, skins, skeletons and clips, with
 * `meshSkeleton` / `animationSkeleton` tying them together. Materials and
 * textures are not surfaced.
 *
 * @param {string} path
 * @returns {MeshGltfScene}
 *
 * @example
 *   const g = Mesh.loadGLTF('char.glb');
 *   const mesh = g.meshes[0];
 *   const skel = g.skeletons[g.meshSkeleton[0]];
 *   const idle = g.animations[0];
 */
Mesh.loadGLTF = function(path) {};

/**
 *  Write the mesh. With `skeleton` it is a rigged asset, and `animations` may
 *  come along; with no options it is plain unskinned geometry.
 * @param {string} path
 * @param {Object} [opts]
 * @param {SkinData} [opts.skin]
 * @param {Skeleton} [opts.skeleton]
 * @param {Array<AnimationClip>} [opts.animations]
 * @returns {boolean}
 */
Mesh.prototype.saveGLTF = function(path, opts) {};
