// rigging.js — the public shape of bro.rigging.SkinData, bro.rigging.Skeleton,
// bro.rigging.Pose, bro.rigging.Animation, bro.rigging.RigSpec, bro.rigging.VoxelChunk,
// bro.rigging.IK, bro.rigging.Rig, and Mesh rigging methods, over __bro_native.rigging.

(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });

    const EMPTY_F32 = new Float32Array(0);
    const EMPTY_U32 = new Uint32Array(0);
    const EMPTY_F64 = new Float64Array(0);
    const EMPTY_U8 = new Uint8Array(0);

    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    const toF32 = (v) => v instanceof Float32Array ? v : Float32Array.from(v);
    const toU32 = (v) => v instanceof Uint32Array ? v : Uint32Array.from(v);
    const toU8  = (v) => v instanceof Uint8Array ? v : Uint8Array.from(v);
    const toF64 = (v) => v instanceof Float64Array ? v : Float64Array.from(v);

    // ---- bro.rigging.SkinData ------------------------------------------------
    function SkinData(opts) {
        const d = opts || {};
        const bw = d.boneWeights ? toF32(d.boneWeights) : (d.weights ? toF32(d.weights) : EMPTY_F32);
        const bi = d.boneIndices ? toU32(d.boneIndices) : (d.indices ? toU32(d.indices) : EMPTY_U32);
        const ibm = d.inverseBindMatrices ? toF32(d.inverseBindMatrices) : EMPTY_F32;
        const bc = d.boneCount !== undefined ? d.boneCount : -1;
        return new __bro_native.rigging.SkinData(bw, bi, ibm, bc);
    }
    {
        const proto = __bro_native.rigging.SkinDataProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, SkinData.prototype);
    }
    fn(mount(bro, "rigging"), "SkinData", SkinData);

    accessor(SkinData.prototype, "boneWeights", function () {
        return __bro_native.rigging.SkinData_boneWeights(this);
    });
    accessor(SkinData.prototype, "boneIndices", function () {
        return __bro_native.rigging.SkinData_boneIndices(this);
    });
    accessor(SkinData.prototype, "inverseBindMatrices", function () {
        return __bro_native.rigging.SkinData_inverseBindMatrices(this);
    });
    accessor(SkinData.prototype, "boneCount", function () {
        return __bro_native.rigging.SkinData_boneCount_get(this);
    });
    accessor(SkinData.prototype, "vertexCount", function () {
        return __bro_native.rigging.SkinData_vertexCount_get(this);
    });
    accessor(SkinData.prototype, "maxWeights", function () {
        return 4;
    });

    fn(SkinData.prototype, "normalize", function normalize() {
        __bro_native.rigging.SkinData_normalize(this);
        return this;
    });
    fn(SkinData.prototype, "clone", function clone() {
        return __bro_native.rigging.SkinData_clone(this);
    });
    fn(SkinData.prototype, "validate", function validate() {
        return SkinData.validate(null, this);
    });

    fn(SkinData, "validate", function validate(mesh, skin, opts) {
        const d = opts || {};
        const influences = d.influences !== undefined ? d.influences : 4;
        const sumTol = d.sumTolerance !== undefined ? d.sumTolerance : 1e-3;
        return JSON.parse(__bro_native.rigging.SkinData_validate(mesh, skin, influences, sumTol));
    });
    fn(SkinData, "transfer", function transfer(targetMesh, sourceMesh, sourceSkin, maxDistance) {
        return __bro_native.rigging.SkinData_transfer(targetMesh, sourceMesh, sourceSkin,
                                                      maxDistance !== undefined ? maxDistance : 0.0);
    });

    // ---- bro.rigging.Skeleton ------------------------------------------------
    function Skeleton(opts) {
        const d = opts || {};
        const json = JSON.stringify(d);
        return new __bro_native.rigging.Skeleton(json);
    }
    {
        const proto = __bro_native.rigging.SkeletonProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, Skeleton.prototype);
    }
    fn(mount(bro, "rigging"), "Skeleton", Skeleton);

    fn(Skeleton, "fromBones", function fromBones(bones) {
        return new Skeleton({ bones: bones });
    });

    accessor(Skeleton.prototype, "boneCount", function () {
        return __bro_native.rigging.Skeleton_boneCount_get(this);
    });
    accessor(Skeleton.prototype, "socketCount", function () {
        return __bro_native.rigging.Skeleton_socketCount_get(this);
    });
    accessor(Skeleton.prototype, "bones", function () {
        return JSON.parse(__bro_native.rigging.Skeleton_bonesJSON(this));
    });
    accessor(Skeleton.prototype, "sockets", function () {
        return JSON.parse(__bro_native.rigging.Skeleton_socketsJSON(this));
    });

    fn(Skeleton.prototype, "findBone", function findBone(name) {
        if (name === undefined) throw new TypeError("Skeleton.findBone: name is required");
        return __bro_native.rigging.Skeleton_findBone(this, name);
    });
    fn(Skeleton.prototype, "findSocket", function findSocket(name) {
        if (name === undefined) throw new TypeError("Skeleton.findSocket: name is required");
        return __bro_native.rigging.Skeleton_findSocket(this, name);
    });
    fn(Skeleton.prototype, "addSocket", function addSocket(s) {
        if (!s || typeof s !== 'object') throw new TypeError("Skeleton.addSocket: object required");
        const off = s.offset ? toF32(s.offset) : EMPTY_F32;
        return __bro_native.rigging.Skeleton_addSocket(this, s.name || "", s.bone || 0, off);
    });
    fn(Skeleton.prototype, "findBoneBySuffix", function (suffix) {
        return __bro_native.rigging.Skeleton_findBoneBySuffix(this, String(suffix));
    });
    fn(Skeleton.prototype, "bindPose", function bindPose() {
        return __bro_native.rigging.Skeleton_bindPose(this);
    });
    fn(Skeleton.prototype, "clone", function clone() {
        return __bro_native.rigging.Skeleton_clone(this);
    });

    // ---- bro.rigging.Pose ----------------------------------------------------
    function Pose(arg) {
        if (typeof arg === 'number') {
            return new __bro_native.rigging.Pose(EMPTY_F32, arg);
        } else if (arg instanceof Float32Array) {
            return new __bro_native.rigging.Pose(arg, 0);
        } else if (arg && arg.data instanceof Float32Array) {
            return new __bro_native.rigging.Pose(arg.data, 0);
        } else {
            return new __bro_native.rigging.Pose(EMPTY_F32, 0);
        }
    }
    {
        const proto = __bro_native.rigging.PoseProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, Pose.prototype);
    }
    fn(mount(bro, "rigging"), "Pose", Pose);

    accessor(Pose.prototype, "data",
        function () {
            return __bro_native.rigging.Pose_data_get(this);
        },
        function (v) {
            __bro_native.rigging.Pose_data_set(this, toF32(v));
        }
    );
    accessor(Pose.prototype, "boneCount", function () {
        return __bro_native.rigging.Pose_boneCount_get(this);
    });

    fn(Pose.prototype, "computeWorldMatrices", function computeWorldMatrices(skel) {
        if (!skel) throw new TypeError("Pose.computeWorldMatrices: Skeleton is required");
        return __bro_native.rigging.Pose_computeWorldMatrices(this, skel);
    });
    fn(Pose.prototype, "computeSkinningMatrices", function computeSkinningMatrices(skel) {
        if (!skel) throw new TypeError("Pose.computeSkinningMatrices: Skeleton is required");
        return __bro_native.rigging.Pose_computeSkinningMatrices(this, skel);
    });
    fn(Pose.prototype, "socketWorld", function socketWorld(skel, name) {
        if (!skel) throw new TypeError("Pose.socketWorld: Skeleton is required");
        const res = __bro_native.rigging.Pose_socketWorld(this, skel, name || "");
        return (res && res.length === 16) ? res : null;
    });
    fn(Pose.prototype, "clone", function clone() {
        return __bro_native.rigging.Pose_clone(this);
    });

    fn(Pose, "blend", function blend(a, b, weight, mask) {
        if (!a || !b) throw new TypeError("Pose.blend: two Pose instances required");
        const m = (mask instanceof Uint8Array) ? mask : (Array.isArray(mask) ? new Uint8Array(mask) : EMPTY_U8);
        __bro_native.rigging.Pose_blend(a, b, weight, m);
        return a;
    });

    fn(Pose, "blendN", function blendN(poses, weights, mask) {
        if (!Array.isArray(poses) || poses.length === 0) {
            throw new TypeError("Pose.blendN: poses array required");
        }
        const n = poses.length;
        const w = (weights instanceof Float32Array) ? weights : new Float32Array(weights);
        if (w.length !== n) {
            throw new TypeError("Pose.blendN: weights must match poses length");
        }
        const singleLen = (poses[0] && poses[0].data) ? poses[0].data.length : 0;
        const allData = new Float32Array(n * singleLen);
        for (let i = 0; i < n; i++) {
            const p = poses[i];
            const pData = (p && p.data) ? p.data : EMPTY_F32;
            allData.set(pData, i * singleLen);
        }
        const m = (mask instanceof Uint8Array) ? mask : (Array.isArray(mask) ? new Uint8Array(mask) : EMPTY_U8);
        const outData = __bro_native.rigging.Pose_blendN(allData, n, w, m);
        return new Pose(outData);
    });

    // ---- bro.rigging.Animation & SkeletalAnimation ---------------------------
    function Animation(opts) {
        const d = opts || {};
        const safe = {
            name: d.name,
            duration: d.duration,
            channels: (d.channels || []).map(ch => ({
                boneIndex: ch.boneIndex,
                path: ch.path,
                interp: ch.interp,
                times: ch.times ? Array.from(ch.times) : [],
                values: ch.values ? Array.from(ch.values) : []
            }))
        };
        return new __bro_native.rigging.Animation(JSON.stringify(safe));
    }
    {
        const proto = __bro_native.rigging.AnimationProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, Animation.prototype);
    }
    fn(mount(bro, "rigging"), "Animation", Animation);
    fn(mount(bro, "rigging"), "SkeletalAnimation", Animation);

    accessor(Animation.prototype, "name",
        function () { return __bro_native.rigging.Animation_name_get(this); },
        function (v) { __bro_native.rigging.Animation_name_set(this, String(v)); }
    );
    accessor(Animation.prototype, "duration",
        function () { return __bro_native.rigging.Animation_duration_get(this); },
        function (v) { __bro_native.rigging.Animation_duration_set(this, Number(v)); }
    );
    accessor(Animation.prototype, "channelCount", function () {
        return __bro_native.rigging.Animation_channelCount_get(this);
    });
    accessor(Animation.prototype, "channels", function () {
        const raw = JSON.parse(__bro_native.rigging.Animation_channelsJSON(this));
        return raw.map(ch => ({
            boneIndex: ch.boneIndex,
            path: ch.path,
            interp: ch.interp,
            times: Float32Array.from(ch.times),
            values: Float32Array.from(ch.values)
        }));
    });

    fn(Animation.prototype, "evaluate", function evaluate(skel, t, opts) {
        if (!skel) throw new TypeError("Animation.evaluate: Skeleton is required");
        const d = opts || {};
        const loop = d.loop !== undefined ? !!d.loop : true;
        return __bro_native.rigging.Animation_evaluate(this, skel, Number(t), loop);
    });
    fn(Animation.prototype, "evaluateInto", function evaluateInto(skel, t, pose, opts) {
        if (!skel || !pose) throw new TypeError("Animation.evaluateInto: Skeleton and Pose required");
        const d = opts || {};
        const loop = d.loop !== undefined ? !!d.loop : true;
        __bro_native.rigging.Animation_evaluateInto(this, skel, Number(t), loop, pose);
        return pose;
    });

    fn(Animation, "retarget", function retarget(anim, srcSkel, dstSkel) {
        return __bro_native.rigging.Animation_retarget(anim, srcSkel, dstSkel);
    });

    // ---- bro.rigging.RigSpec -------------------------------------------------
    function RigSpec(name) {
        return new __bro_native.rigging.RigSpec(name || "");
    }
    {
        const proto = __bro_native.rigging.RigSpecProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, RigSpec.prototype);
    }
    fn(mount(bro, "rigging"), "RigSpec", RigSpec);

    accessor(RigSpec.prototype, "name", function () {
        return __bro_native.rigging.RigSpec_name_get(this);
    });
    accessor(RigSpec.prototype, "boneCount", function () {
        return __bro_native.rigging.RigSpec_boneCount_get(this);
    });
    accessor(RigSpec.prototype, "landmarkCount", function () {
        return __bro_native.rigging.RigSpec_landmarkCount_get(this);
    });

    fn(RigSpec.prototype, "toJSON", function toJSON() {
        return __bro_native.rigging.RigSpec_toJSON(this);
    });
    fn(RigSpec.prototype, "landmarkNames", function landmarkNames() {
        return JSON.parse(__bro_native.rigging.RigSpec_landmarkNames(this));
    });
    fn(RigSpec.prototype, "boneNames", function boneNames() {
        return JSON.parse(__bro_native.rigging.RigSpec_boneNames(this));
    });

    // ---- bro.rigging.VoxelChunk ----------------------------------------------
    function VoxelChunk(dimX, dimY, dimZ, cellSize) {
        return new __bro_native.rigging.VoxelChunk(dimX, dimY, dimZ, cellSize !== undefined ? cellSize : 1.0);
    }
    {
        const proto = __bro_native.rigging.VoxelChunkProto;
        if (proto !== undefined) Object.setPrototypeOf(proto, VoxelChunk.prototype);
    }
    fn(mount(bro, "rigging"), "VoxelChunk", VoxelChunk);

    accessor(VoxelChunk.prototype, "sizeX", function () {
        return __bro_native.rigging.VoxelChunk_sizeX_get(this);
    });
    accessor(VoxelChunk.prototype, "sizeY", function () {
        return __bro_native.rigging.VoxelChunk_sizeY_get(this);
    });
    accessor(VoxelChunk.prototype, "sizeZ", function () {
        return __bro_native.rigging.VoxelChunk_sizeZ_get(this);
    });
    accessor(VoxelChunk.prototype, "cellSize", function () {
        return __bro_native.rigging.VoxelChunk_cellSize_get(this);
    });
    accessor(VoxelChunk.prototype, "isDirty",
        function () { return __bro_native.rigging.VoxelChunk_isDirty_get(this); },
        function (v) { __bro_native.rigging.VoxelChunk_isDirty_set(this, !!v); }
    );

    fn(VoxelChunk.prototype, "setVoxel", function setVoxel(x, y, z, val) {
        __bro_native.rigging.VoxelChunk_setVoxel(this, x, y, z, val);
    });
    fn(VoxelChunk.prototype, "getVoxel", function getVoxel(x, y, z) {
        return __bro_native.rigging.VoxelChunk_getVoxel(this, x, y, z);
    });
    fn(VoxelChunk.prototype, "fill", function fill(val) {
        __bro_native.rigging.VoxelChunk_fill(this, val);
    });
    fn(VoxelChunk.prototype, "markDirty", function markDirty() {
        __bro_native.rigging.VoxelChunk_markDirty(this);
    });
    fn(VoxelChunk.prototype, "clearDirty", function clearDirty() {
        __bro_native.rigging.VoxelChunk_clearDirty(this);
    });
    fn(VoxelChunk.prototype, "data", function data() {
        return __bro_native.rigging.VoxelChunk_data(this);
    });
    fn(VoxelChunk.prototype, "setData", function setData(buf) {
        __bro_native.rigging.VoxelChunk_setData(this, toU8(buf));
    });
    fn(VoxelChunk.prototype, "buildMesh", function buildMesh(palette, count) {
        const pal = palette ? toF32(palette) : EMPTY_F32;
        const cnt = count !== undefined ? count : 0;
        return __bro_native.rigging.VoxelChunk_buildMesh(this, pal, cnt);
    });

    // ---- bro.rigging.IK ------------------------------------------------------
    function IK() {
        throw new TypeError("IK is not constructible");
    }
    fn(mount(bro, "rigging"), "IK", IK);

    fn(IK, "twoBone", function twoBone(skel, pose, root, mid, end, target, pole) {
        if (!skel || !pose || !target) throw new TypeError("IK.twoBone: skel, pose, and target required");
        return __bro_native.rigging.IK_twoBone(skel, pose, root, mid, end,
                                               toF64(target), pole ? toF64(pole) : EMPTY_F64);
    });
    fn(IK, "FABRIK", function FABRIK(skel, pose, chain, target, opts) {
        if (!skel || !pose || !chain || !target) throw new TypeError("IK.FABRIK: skel, pose, chain, target required");
        const d = opts || {};
        const iters = d.iterations !== undefined ? d.iterations : 10;
        const tol = d.tolerance !== undefined ? d.tolerance : 1e-3;
        const ch = chain instanceof Int32Array ? chain : Int32Array.from(chain);
        return __bro_native.rigging.IK_FABRIK(skel, pose, ch, toF64(target), iters, tol);
    });
    fn(IK, "lookAt", function lookAt(skel, pose, bone, target, opts) {
        if (!skel || !pose || !target) throw new TypeError("IK.lookAt: skel, pose, target required");
        const d = opts || {};
        const fwd = d.forward ? toF64(d.forward) : EMPTY_F64;
        const up = d.up ? toF64(d.up) : EMPTY_F64;
        return __bro_native.rigging.IK_lookAt(skel, pose, bone, toF64(target), fwd, up);
    });

    // Backward-compat aliases
    fn(IK, "solveTwoBone", function solveTwoBone(opts) {
        const d = opts || {};
        return IK.twoBone(d.skel, d.pose, d.root || 0, d.mid || 1, d.end || 2, d.targetPos, d.poleVector);
    });
    fn(IK, "solveFabrik", function solveFabrik(opts) {
        const d = opts || {};
        return IK.FABRIK(d.skel, d.pose, d.chain, d.targetPos, { iterations: d.maxIterations, tolerance: d.tolerance });
    });
    fn(IK, "solveLookAt", function solveLookAt(opts) {
        const d = opts || {};
        return IK.lookAt(d.skel, d.pose, d.bone || 0, d.targetPos, { forward: d.forward, up: d.up });
    });

    // ---- bro.rigging.Rig -----------------------------------------------------
    function Rig() {
        throw new TypeError("Rig is not constructible");
    }
    fn(mount(bro, "rigging"), "Rig", Rig);

    fn(Rig, "spec", function spec(name) {
        return __bro_native.rigging.Rig_spec(name || "");
    });
    fn(Rig, "specFromJSON", function specFromJSON(json) {
        return __bro_native.rigging.Rig_specFromJSON(json || "");
    });
    fn(Rig, "specFromFile", function specFromFile(path) {
        return __bro_native.rigging.Rig_specFromFile(path || "");
    });
    fn(Rig, "detectHumanoid", function detectHumanoid(mesh) {
        if (!mesh) throw new TypeError("Rig.detectHumanoid: mesh required");
        return JSON.parse(__bro_native.rigging.Rig_detectHumanoid(mesh));
    });
    fn(Rig, "detectQuadruped", function detectQuadruped(mesh) {
        if (!mesh) throw new TypeError("Rig.detectQuadruped: mesh required");
        return JSON.parse(__bro_native.rigging.Rig_detectQuadruped(mesh));
    });
    fn(Rig, "missingLandmarks", function missingLandmarks(spec, partial) {
        if (!spec) throw new TypeError("Rig.missingLandmarks: spec required");
        return JSON.parse(__bro_native.rigging.Rig_missingLandmarks(spec, JSON.stringify(partial || {})));
    });
    fn(Rig, "fitSkeleton", function fitSkeleton(spec, lm, mesh) {
        if (!spec || !mesh) throw new TypeError("Rig.fitSkeleton: spec and mesh required");
        return __bro_native.rigging.Rig_fitSkeleton(spec, JSON.stringify(lm || {}), mesh);
    });
    fn(Rig, "autoRig", function autoRig(mesh, spec, lm, opts) {
        if (!mesh || !spec) throw new TypeError("Rig.autoRig: mesh and spec required");
        __bro_native.rigging.Rig_autoRig(mesh, spec, JSON.stringify(lm || {}), JSON.stringify(opts || {}));
        return {
            skeleton: __bro_native.rigging.Rig_autoRig_skeleton(),
            skin: __bro_native.rigging.Rig_autoRig_skin(),
            methodUsed: __bro_native.rigging.Rig_autoRig_methodUsed(),
            missingLandmarks: JSON.parse(__bro_native.rigging.Rig_autoRig_missingLandmarks()),
            warnings: JSON.parse(__bro_native.rigging.Rig_autoRig_warnings())
        };
    });
    fn(Rig, "generateLocomotionCycle", function generateLocomotionCycle(skel, spec, opts) {
        if (!skel || !spec) throw new TypeError("Rig.generateLocomotionCycle: skel and spec required");
        return __bro_native.rigging.Rig_generateLocomotionCycle(skel, spec, JSON.stringify(opts || {}));
    });
    fn(Rig, "transferWeights", function transferWeights(sourceMesh, sourceSkin, targetMesh) {
        if (!sourceMesh || !sourceSkin || !targetMesh)
            throw new TypeError("Rig.transferWeights: sourceMesh, sourceSkin, targetMesh required");
        return __bro_native.rigging.Rig_transferWeights(targetMesh, sourceMesh, sourceSkin);
    });

    // ---- Decorate Mesh with Rigging extensions ------------------------------
    const MeshClass = globalThis.Mesh || (bro && bro.mesh && bro.mesh.Mesh);
    if (MeshClass && MeshClass.prototype) {
        fn(MeshClass.prototype, "applySkinning", function applySkinning(skin, poseMatrices) {
            if (skin === undefined) throw new TypeError("Mesh.applySkinning: skin is required");
            if (poseMatrices === undefined) throw new TypeError("Mesh.applySkinning: poseMatrices is required");
            const pm = poseMatrices instanceof Float32Array ? poseMatrices : Float32Array.from(poseMatrices);
            __bro_native.rigging.Mesh_applySkinning(this, skin, pm);
            return this;
        });

        fn(MeshClass.prototype, "applyMorphTarget", function applyMorphTarget(target, weight) {
            if (target === undefined) throw new TypeError("Mesh.applyMorphTarget: target is required");
            if (weight === undefined) throw new TypeError("Mesh.applyMorphTarget: weight is required");
            const name = target.name || "";
            const dp = target.deltaPositions ? toF32(target.deltaPositions) : EMPTY_F32;
            const dn = target.deltaNormals ? toF32(target.deltaNormals) : EMPTY_F32;
            __bro_native.rigging.Mesh_applyMorphTarget(this, name, dp, dn, Number(weight));
            return this;
        });

        fn(MeshClass.prototype, "saveGLTF", function saveGLTF(path, opts) {
            if (path === undefined) throw new TypeError("Mesh.saveGLTF: path is required");
            const d = opts || {};
            const skinHandle = d.skin || null;
            const skelHandle = d.skeleton || null;
            const animsJson = (d.animations && Array.isArray(d.animations)) ? JSON.stringify(d.animations.map(a => {
                const rawChannels = typeof a.channels === 'function' ? a.channels() : (a.channels || []);
                return {
                    name: a.name || "",
                    duration: a.duration || 0,
                    channels: rawChannels.map(ch => ({
                        boneIndex: ch.boneIndex,
                        path: ch.path,
                        interp: ch.interp,
                        times: ch.times ? Array.from(ch.times) : [],
                        values: ch.values ? Array.from(ch.values) : []
                    }))
                };
            })) : "";
            return __bro_native.rigging.Mesh_saveGLTFRigged(this, skinHandle, skelHandle, animsJson, path);
        });

        fn(MeshClass, "loadGLTF", function loadGLTF(path) {
            if (path === undefined) throw new TypeError("Mesh.loadGLTF: path is required");
            if (!__bro_native.rigging.Mesh_loadGLTF(path)) {
                return {
                    meshes: [],
                    skins: [],
                    skeletons: [],
                    animations: [],
                    meshSkeleton: [],
                    animationSkeleton: []
                };
            }
            const meshCount = Math.floor(__bro_native.rigging.Mesh_loadGLTF_meshCount());
            const meshes = new Array(meshCount);
            for (let i = 0; i < meshCount; i++) {
                meshes[i] = __bro_native.rigging.Mesh_loadGLTF_takeMesh(i);
            }
            const skinCount = Math.floor(__bro_native.rigging.Mesh_loadGLTF_skinCount());
            const skins = new Array(skinCount);
            for (let i = 0; i < skinCount; i++) {
                skins[i] = __bro_native.rigging.Mesh_loadGLTF_takeSkin(i);
            }
            const skelCount = Math.floor(__bro_native.rigging.Mesh_loadGLTF_skeletonCount());
            const skeletons = new Array(skelCount);
            for (let i = 0; i < skelCount; i++) {
                skeletons[i] = __bro_native.rigging.Mesh_loadGLTF_takeSkeleton(i);
            }
            const animCount = Math.floor(__bro_native.rigging.Mesh_loadGLTF_animationCount());
            const animations = new Array(animCount);
            for (let i = 0; i < animCount; i++) {
                animations[i] = __bro_native.rigging.Mesh_loadGLTF_takeAnimation(i);
            }
            const meshSkeleton = Array.from(__bro_native.rigging.Mesh_loadGLTF_meshSkeleton());
            const animationSkeleton = Array.from(__bro_native.rigging.Mesh_loadGLTF_animationSkeleton());
            return {
                meshes,
                skins,
                skeletons,
                animations,
                meshSkeleton,
                animationSkeleton
            };
        });
    }

    // ---- Global Exports -----------------------------------------------------
    globalThis.SkinData = SkinData;
    globalThis.Skeleton = Skeleton;
    globalThis.Pose = Pose;
    globalThis.Animation = Animation;
    globalThis.SkeletalAnimation = Animation;
    globalThis.RigSpec = RigSpec;
    globalThis.VoxelChunk = VoxelChunk;
    globalThis.IK = IK;
    globalThis.Rig = Rig;
})();
