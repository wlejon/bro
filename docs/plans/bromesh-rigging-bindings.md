# Plan: bromesh rigging & animation JS bindings

Bind the rigging/animation subsystem of bromesh — the part intentionally deferred
when the mesh-level API was expanded in commit `cf039d2`. The prior pass covered
geometry, CSG, isosurface, baking, BVH, progressive mesh, meshlets, encoding,
UV metrics, and analysis. None of the skeletal/animation types are reachable
from JS today.

## Starting state

- `src/js/mesh_bindings.cpp` defines `Mesh`, `MeshBVH`, `ProgressiveMesh`.
- `Mesh.loadGLTF(path)` returns `{ meshes: Mesh[] }` only — the `GltfScene`'s
  `skins`, `skeletons`, `animations`, and `meshSkeleton` / `animationSkeleton`
  indices are discarded at the binding layer (see `loadGLTF` lambda near the
  bottom of `mesh_bindings.cpp`).
- Tests live in `tests/mesh/` (10 files, all passing).
- Docs live in `docs/mesh-api.js` (JSDoc-annotated API reference).
- All JS bindings use qjsbind (see `feedback_use_qjsbind` memory).

## Scope — what to bind

bromesh rigging headers to cover (all under `D:/projects/bromesh/include/bromesh/`):

| Header | What's there |
|--------|--------------|
| `mesh_data.h` (tail) | `Bone`, `Socket`, `Skeleton`, `AnimChannel`, `Animation`, `SkinData`, `MorphTarget` structs |
| `animation/pose.h` | `Pose`, `bindPose`, `evaluateAnimation`, `evaluateAnimationInto`, `blendPoses`, `computeWorldMatrices`, `computeSkinningMatrices`, `socketWorldMatrix` |
| `animation/ik.h` | `solveTwoBoneIK`, `solveFABRIK`, `solveLookAt` |
| `animation/retarget.h` | `retargetAnimation`, `addRigifySockets`, `findBoneBySuffix` |
| `animation/locomotion.h` | `LegChain`, `GaitPattern`, `LocomotionParams`, `identifyLegChains`, `defaultGait`, `generateLocomotionCycle` |
| `rigging/rig_spec.h` | `RigSpec`, builtin humanoid/quadruped/hexapod/octopod, JSON parse/serialize |
| `rigging/landmarks.h` + `landmark_detect.h` | `Landmarks`, `detectHumanoidLandmarks`, `detectQuadrupedLandmarks`, `missingLandmarks` |
| `rigging/skeleton_fit.h` | `fitSkeleton` |
| `rigging/weighting.h` + `bone_heat.h` + `bbw.h` + `voxel_bind.h` + `weight_smooth.h` + `skin_validate.h` | weighting methods, options, validation |
| `rigging/auto_rig.h` | high-level `autoRig` entry point + `AutoRigResult` |
| `manipulation/skin.h` + `skin_transfer.h` | `applySkinning`, `applyMorphTarget`, `normalizeWeights`, `transferSkinWeights` |
| `voxel/voxel_chunk.h` | `VoxelChunk` class (small — bind while in the area) |

Also: extend `Mesh.loadGLTF` return value to include `{ meshes, skins, skeletons, animations, meshSkeleton, animationSkeleton }`, and add a `saveGLTF` overload that accepts skin/skeleton/animations.

## Suggested JS surface

Four new JS classes + one data-object convention for `Pose`:

### `Skeleton`
Wraps `bromesh::Skeleton`. JSON-like bones + sockets.
```js
const skel = new Skeleton();           // empty
const skel = Skeleton.fromBones(arr);  // [{name, parent, localT, localR, localS, inverseBind}]
skel.bones;        // array of bone descriptors (readonly copy)
skel.sockets;      // array of socket descriptors
skel.findBone(name);         // number, -1 if missing
skel.findSocket(name);       // number, -1 if missing
skel.boneCount;              // readonly
skel.socketCount;            // readonly
skel.addSocket({ name, bone, offset });
skel.bindPose();             // returns a Pose (flat Float32Array, 10 floats/bone)
```

### `Animation`
Wraps `bromesh::Animation`. Mostly data.
```js
const a = new Animation({ name, duration, channels });
a.name; a.duration; a.channels;   // channels = [{boneIndex, path, interp, times, values}]
a.evaluate(skeleton, t, { loop: true });  // -> Pose
Animation.retarget(anim, srcSkel, dstSkel);  // static -> new Animation
```

### `Pose`
Lightweight — could be a plain object `{ data: Float32Array, boneCount }` instead of a class, since its only behaviour is blend + world/skinning matrix computation. Recommend a class for clean lifecycle:
```js
const pose = skel.bindPose();
Pose.blend(a, b, weight, boneMask?);   // in-place on `a`
pose.computeWorldMatrices(skel);       // -> Float32Array (16 * boneCount)
pose.computeSkinningMatrices(skel);    // -> Float32Array (16 * boneCount)
pose.socketWorld(skel, name);          // -> Float32Array(16) or null
```

### `SkinData`
Wraps `bromesh::SkinData`. Mostly just typed arrays.
```js
const skin = new SkinData({ boneWeights, boneIndices, inverseBindMatrices, boneCount });
skin.boneWeights;         // Float32Array, stride 4
skin.boneIndices;         // Uint32Array, stride 4
skin.inverseBindMatrices; // Float32Array, 16 * boneCount
skin.boneCount;
skin.normalize();         // normalizeWeights in-place
SkinData.validate(mesh, skin, { influences: 4, sumTolerance: 1e-3 });
SkinData.transfer(targetMesh, sourceMesh, sourceSkin, maxDistance?);
```

### IK as free functions on a namespace object
IK mutates a pose in-place — simplest as methods on the pose or a global `IK`:
```js
IK.twoBone(skel, pose, rootBone, midBone, endBone, targetWorld, poleWorld?);
IK.FABRIK(skel, pose, chain, targetWorld, { iterations: 10, tolerance: 1e-3 });
IK.lookAt(skel, pose, bone, targetWorld, { localForward, localUp });
```

### Rigging pipeline as a namespace
One-shot functions — no class state needed:
```js
const spec = Rig.spec('humanoid');            // or Rig.spec('quadruped') / .specFromJSON(str) / .specFromFile(path)
const landmarks = Rig.detectHumanoid(mesh);   // -> plain object { "head": [x,y,z], ... }
const missing   = Rig.missingLandmarks(spec, landmarks);
const skel      = Rig.fitSkeleton(spec, landmarks, mesh);
const result    = Rig.autoRig(mesh, spec, landmarks, { method: 'boneHeat', ... });
// result = { skeleton: Skeleton, skin: SkinData, missingLandmarks: [], warnings: [], methodUsed: 'boneHeat' }

const cycle = Rig.generateLocomotionCycle(skel, spec, {
    gait: { name, phases, dutyFactor }, strideLength, cycleDuration, ...
});
// cycle is an Animation
```

### Mesh extensions (instance methods)
```js
mesh.applySkinning(skin, poseMatrices);   // Float32Array of world matrices
mesh.applyMorphTarget({ name, deltaPositions, deltaNormals? }, weight);
```

### VoxelChunk class
```js
const chunk = new VoxelChunk(sizeX, sizeY, sizeZ, cellSize?);
chunk.sizeX; chunk.sizeY; chunk.sizeZ; chunk.cellSize;
chunk.getVoxel(x, y, z);
chunk.setVoxel(x, y, z, material);
chunk.data();                // returns Uint8Array view (copy)
chunk.setData(arr);          // bulk set from Uint8Array
chunk.fill(value);
chunk.isDirty; chunk.markDirty(); chunk.clearDirty();
chunk.buildMesh(palette?, paletteCount?);  // -> Mesh
```

## Implementation order

Do these in sequence — later steps depend on types from earlier ones.

1. **Skin types + VoxelChunk** (lowest-risk, pure data classes).
   - `SkinData` class, with typed-array get/set like `Mesh` already does.
   - `MorphTarget` as a plain JS object (no class needed — just `{ name, deltaPositions, deltaNormals? }` consumed by `mesh.applyMorphTarget`).
   - `VoxelChunk` class.
   - `mesh.applySkinning`, `mesh.applyMorphTarget`.
   - Test: apply an identity pose's skinning matrices → mesh positions unchanged; apply a simple morph target → positions shift.

2. **Skeleton + Pose**.
   - `Skeleton` class with bones/sockets as arrays of plain JS objects.
   - Transit format for bones: `{ name, parent, localT:[3], localR:[4], localS:[3], inverseBind:[16] }`.
   - `Pose` class (or object). `computeWorldMatrices` / `computeSkinningMatrices` return `Float32Array`.
   - Test: build a 2-bone skeleton, bindPose, compute world matrices, verify root == identity and child has expected translation.

3. **Animation**.
   - `Animation` class with `channels` as an array of `{ boneIndex, path: "translation"|"rotation"|"scale", interp: "linear"|"step"|"cubicSpline", times: Float32Array, values: Float32Array }`.
   - `anim.evaluate(skel, t, {loop})` returns a new Pose.
   - `Animation.retarget(anim, src, dst)`.
   - Test: one-channel rotation animation, evaluate at several `t` values, compare to expected quaternion slerp.

4. **IK namespace**.
   - Bind the three solvers as methods on a global `IK` object (register via `qjsbind::global_object`).
   - Test: 2-bone chain, place target, solve, verify end-effector is within tolerance of target.

5. **Rigging pipeline (`Rig` namespace)**.
   - `Rig.spec(name)` / `Rig.specFromJSON` / `Rig.specFromFile` → opaque `RigSpec` wrapper.
   - `Rig.detectHumanoid(mesh)` / `Rig.detectQuadruped(mesh)` → plain object of `{name: [x,y,z]}`.
   - `Rig.missingLandmarks(spec, landmarks)` → string[].
   - `Rig.fitSkeleton(spec, landmarks, mesh)` → Skeleton.
   - `Rig.autoRig(mesh, spec, landmarks, options)` → `{ skeleton, skin, missingLandmarks, warnings, methodUsed }`.
   - `Rig.generateLocomotionCycle` → Animation.
   - Tests need a real humanoid mesh — either ship a small procedural one (stretched capsule assembly) or use `Mesh.capsule()` as a smoke test that just checks the pipeline runs without crashing.

6. **glTF scene extension**.
   - Change `Mesh.loadGLTF(path)` to return `{ meshes, skins, skeletons, animations, meshSkeleton, animationSkeleton }`.
   - Add overload `mesh.saveGLTF(path, { skin?, skeleton?, animations? })`.
   - Load a rigged glTF in a test (need a fixture; `stb_image` bundled rigged samples or check if bromesh ships a test asset).

## Tricky bits / pitfalls

- **Quaternion convention**: bromesh uses xyzw order. Match that in JS — document loudly.
- **Matrix convention**: bromesh is column-major 4x4 (16 floats), matching glTF and `mesh.transform(matrix)`. Stay consistent.
- **BBW requires OSQP**: `bbwWeights` may fail to link or throw if OSQP isn't available. Guard the binding with a runtime fallback that throws a clear JS error instead of aborting.
- **Meshoptimizer in Debug**: see `feedback_bromesh_test_release` memory — Debug builds of mesh-heavy tests can trigger modal abort dialogs. Our test runner uses `--no-gpu` but this still matters; if rigging tests hit it, run them in Release.
- **Skeleton lifetime vs Animation.evaluate**: evaluate takes a const Skeleton& — JS holds a wrapped unique_ptr, so just pass the wrapper through. No refcount plumbing needed.
- **Pose blend is in-place** on the first arg: match that in JS (`Pose.blend(a, b, weight)` mutates `a`). Document clearly.
- **IK expects mutable pose**: same pattern — JS pose objects must be mutable.
- **Landmarks return map**: bromesh uses `std::unordered_map<string, array<float,3>>`. In JS, a plain `{name: [x,y,z]}` object is ergonomic. Round-trip it when accepting landmarks as input.
- **`RigSpec` is a large struct** with nested BoneDecl/LandmarkDecl/SocketDecl. Keep it opaque (wrapper class with `toJSON()` / `Rig.specFromJSON()`); don't try to surface the nested structure as live JS objects.
- **Loop animation**: `evaluateAnimation` has a `loop` bool. Default to `true` in JS to match typical runtime use.

## File layout

Recommended split to keep `mesh_bindings.cpp` from ballooning:

```
src/js/
├── mesh_bindings.cpp          (existing — Mesh, MeshBVH, ProgressiveMesh)
├── mesh_bindings.h
├── rigging_bindings.cpp       (NEW — Skeleton, Pose, Animation, SkinData, IK, Rig)
├── rigging_bindings.h
└── voxel_bindings.cpp         (NEW — VoxelChunk)  // or fold into mesh_bindings
```

Wire `RiggingBindings::install(ctx)` into the JS runtime bootstrap alongside
`MeshBindings::install(ctx)` — see `src/js/runtime.cpp` for the pattern.

Add `bromesh/animation/*.h`, `bromesh/rigging/*.h`, `bromesh/manipulation/skin.h`,
`bromesh/manipulation/skin_transfer.h`, `bromesh/voxel/voxel_chunk.h` to the
includes in whichever binding file owns each class.

## Testing

New `tests/rigging/`:

- `test_skeleton.js` — construct from bones, bindPose, findBone, lookups.
- `test_pose.js` — computeWorldMatrices on 2-bone chain, blend two poses.
- `test_animation.js` — single-channel rotation anim, evaluate at multiple t.
- `test_skin.js` — applySkinning with identity pose, validate, normalize, transfer.
- `test_ik.js` — twoBone, FABRIK, lookAt solvers reach target within tolerance.
- `test_rig.js` — specs load, detect landmarks on a procedural humanoid, autoRig runs end-to-end.
- `test_morph.js` — applyMorphTarget shifts positions correctly.
- `test_voxel_chunk.js` — set/get, fill, buildMesh.
- `test_gltf_rigged.js` — load a rigged glTF, verify skeleton + animation present.

Follow the same pattern as `tests/mesh/` — plain JS files with `assert(cond, msg)`.

## Docs

Update `docs/mesh-api.js` — add sections for `Skeleton`, `Pose`, `Animation`,
`SkinData`, `IK`, `Rig`, `VoxelChunk`. JSDoc style matching the existing file.

Update the opening paragraph to list all classes (currently says three).

## Memory hygiene

When done, update `project_mesh_bindings_state.md` — move the rigging items
from "intentionally NOT bound" to "covered" and record any remaining gaps.

## Estimated scope

~800–1200 lines of binding C++ plus ~400–600 lines of tests plus doc updates.
Roughly 1.5–2× the size of the prior mesh-bindings expansion. Do it in the
order above, commit after each numbered step — don't batch.
