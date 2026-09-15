// rigging.js — the public shape of bro.rigging.SkinData, bro.rigging.Skeleton, bro.rigging.Pose, bro.rigging.SkeletalAnimation, bro.rigging.RigSpec, bro.rigging.VoxelChunk, bro.rigging.IK, bro.rigging.Rig, assembled over the
// natives under __bro_native.rigging.

(function () {
    'use strict';

    const accessor = (obj, name, get, set) =>
        Object.defineProperty(obj, name, { get, set, enumerable: true, configurable: true });
    const fn = (obj, name, value) =>
        Object.defineProperty(obj, name, { value, writable: true, enumerable: true, configurable: true });
    const EMPTY_F32 = new Float32Array(0);
    const EMPTY_F64 = new Float64Array(0);
    const EMPTY_U16 = new Uint16Array(0);
    const mount = (root, name) => root[name] !== undefined ? root[name] : (root[name] = {});
    const toF32 = (v) => v instanceof Float32Array ? v : Float32Array.from(v);
    const toF64 = (v) => v instanceof Float64Array ? v : Float64Array.from(v);
    const toU16 = (v) => v instanceof Uint16Array ? v : Uint16Array.from(v);

    // ---- bro.rigging.SkinData ------------------------------------------------
    function SkinData(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return new __bro_native.rigging.SkinData(d_opts.vertexCount !== undefined, d_opts.vertexCount === undefined ? 0 : d_opts.vertexCount, d_opts.maxWeightsPerVertex !== undefined, d_opts.maxWeightsPerVertex === undefined ? 0 : d_opts.maxWeightsPerVertex, d_opts.indices === undefined ? EMPTY_U16 : toU16(d_opts.indices), d_opts.weights === undefined ? EMPTY_F32 : toF32(d_opts.weights));
    }
    {
        const proto = __bro_native.rigging.SkinDataProto;
        if (proto === undefined) throw new Error("bro.rigging.SkinData: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, SkinData.prototype);
    }
    fn(mount(bro, "rigging"), "SkinData", SkinData);
    accessor(SkinData.prototype, "vertexCount",
        function () {
            return __bro_native.rigging.SkinData_vertexCount_get(this);
        },
        undefined);
    accessor(SkinData.prototype, "maxWeights",
        function () {
            return __bro_native.rigging.SkinData_maxWeights_get(this);
        },
        undefined);
    fn(SkinData.prototype, "clone", function clone() {
        return __bro_native.rigging.SkinData_clone(this);
    });
    fn(SkinData.prototype, "validate", function validate() {
        return __bro_native.rigging.SkinData_validate(this);
    });
    fn(SkinData.prototype, "normalize", function normalize() {
        __bro_native.rigging.SkinData_normalize(this);
    });

    // ---- bro.rigging.Skeleton ------------------------------------------------
    function Skeleton(opts) {
        const d_opts = opts === undefined ? {} : opts;
        return new __bro_native.rigging.Skeleton(d_opts.bones === undefined ? '' : JSON.stringify(d_opts.bones), d_opts.sockets === undefined ? '' : JSON.stringify(d_opts.sockets));
    }
    {
        const proto = __bro_native.rigging.SkeletonProto;
        if (proto === undefined) throw new Error("bro.rigging.Skeleton: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, Skeleton.prototype);
    }
    fn(mount(bro, "rigging"), "Skeleton", Skeleton);
    accessor(Skeleton.prototype, "boneCount",
        function () {
            return __bro_native.rigging.Skeleton_boneCount_get(this);
        },
        undefined);
    fn(Skeleton.prototype, "findBone", function findBone(name) {
        if (name === undefined) throw new TypeError("bro.rigging.Skeleton.prototype.findBone: name is required");
        return __bro_native.rigging.Skeleton_findBone(this, name);
    });
    fn(Skeleton.prototype, "boneName", function boneName(index) {
        if (index === undefined) throw new TypeError("bro.rigging.Skeleton.prototype.boneName: index is required");
        return __bro_native.rigging.Skeleton_boneName(this, index);
    });
    fn(Skeleton.prototype, "boneParent", function boneParent(index) {
        if (index === undefined) throw new TypeError("bro.rigging.Skeleton.prototype.boneParent: index is required");
        return __bro_native.rigging.Skeleton_boneParent(this, index);
    });
    fn(Skeleton.prototype, "boneBindPose", function boneBindPose(index) {
        if (index === undefined) throw new TypeError("bro.rigging.Skeleton.prototype.boneBindPose: index is required");
        return Array.from(__bro_native.rigging.Skeleton_boneBindPose(this, index));
    });
    fn(Skeleton.prototype, "boneInverseBind", function boneInverseBind(index) {
        if (index === undefined) throw new TypeError("bro.rigging.Skeleton.prototype.boneInverseBind: index is required");
        return Array.from(__bro_native.rigging.Skeleton_boneInverseBind(this, index));
    });
    fn(Skeleton.prototype, "clone", function clone() {
        return __bro_native.rigging.Skeleton_clone(this);
    });

    // ---- bro.rigging.Pose ----------------------------------------------------
    function Pose(skeleton) {
        return new __bro_native.rigging.Pose(skeleton);
    }
    {
        const proto = __bro_native.rigging.PoseProto;
        if (proto === undefined) throw new Error("bro.rigging.Pose: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, Pose.prototype);
    }
    fn(mount(bro, "rigging"), "Pose", Pose);
    accessor(Pose.prototype, "boneCount",
        function () {
            return __bro_native.rigging.Pose_boneCount_get(this);
        },
        undefined);
    fn(Pose.prototype, "getBoneLocal", function getBoneLocal(index) {
        if (index === undefined) throw new TypeError("bro.rigging.Pose.prototype.getBoneLocal: index is required");
        __bro_native.rigging.Pose_getBoneLocal(this, index);
        return { translation: Array.from(__bro_native.rigging.Pose_getBoneLocal_translation()), rotation: Array.from(__bro_native.rigging.Pose_getBoneLocal_rotation()), scale: Array.from(__bro_native.rigging.Pose_getBoneLocal_scale()) };
    });
    fn(Pose.prototype, "setBoneLocal", function setBoneLocal(index, trs) {
        if (index === undefined) throw new TypeError("bro.rigging.Pose.prototype.setBoneLocal: index is required");
        if (trs === undefined) throw new TypeError("bro.rigging.Pose.prototype.setBoneLocal: trs is required");
        const d_trs = trs;
        __bro_native.rigging.Pose_setBoneLocal(this, index, d_trs.translation === undefined ? EMPTY_F64 : toF64(d_trs.translation), d_trs.rotation === undefined ? EMPTY_F64 : toF64(d_trs.rotation), d_trs.scale === undefined ? EMPTY_F64 : toF64(d_trs.scale));
    });
    fn(Pose.prototype, "getBoneModelMatrix", function getBoneModelMatrix(index) {
        if (index === undefined) throw new TypeError("bro.rigging.Pose.prototype.getBoneModelMatrix: index is required");
        return Array.from(__bro_native.rigging.Pose_getBoneModelMatrix(this, index));
    });
    fn(Pose.prototype, "clone", function clone() {
        return __bro_native.rigging.Pose_clone(this);
    });

    // ---- bro.rigging.SkeletalAnimation ---------------------------------------
    function SkeletalAnimation() {
        throw new TypeError("bro.rigging.SkeletalAnimation is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.rigging.SkeletalAnimationProto;
        if (proto === undefined) throw new Error("bro.rigging.SkeletalAnimation: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, SkeletalAnimation.prototype);
    }
    fn(mount(bro, "rigging"), "SkeletalAnimation", SkeletalAnimation);
    accessor(SkeletalAnimation.prototype, "name",
        function () {
            return __bro_native.rigging.SkeletalAnimation_name_get(this);
        },
        undefined);
    accessor(SkeletalAnimation.prototype, "duration",
        function () {
            return __bro_native.rigging.SkeletalAnimation_duration_get(this);
        },
        undefined);
    accessor(SkeletalAnimation.prototype, "trackCount",
        function () {
            return __bro_native.rigging.SkeletalAnimation_trackCount_get(this);
        },
        undefined);
    fn(SkeletalAnimation.prototype, "evaluate", function evaluate(time, outPose) {
        if (time === undefined) throw new TypeError("bro.rigging.SkeletalAnimation.prototype.evaluate: time is required");
        if (outPose === undefined) throw new TypeError("bro.rigging.SkeletalAnimation.prototype.evaluate: outPose is required");
        __bro_native.rigging.SkeletalAnimation_evaluate(this, time, outPose);
    });

    // ---- bro.rigging.RigSpec -------------------------------------------------
    function RigSpec(type) {
        return new __bro_native.rigging.RigSpec(type);
    }
    {
        const proto = __bro_native.rigging.RigSpecProto;
        if (proto === undefined) throw new Error("bro.rigging.RigSpec: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, RigSpec.prototype);
    }
    fn(mount(bro, "rigging"), "RigSpec", RigSpec);
    accessor(RigSpec.prototype, "type",
        function () {
            return __bro_native.rigging.RigSpec_type_get(this);
        },
        undefined);
    accessor(RigSpec.prototype, "requiredBones",
        function () {
            return JSON.parse(__bro_native.rigging.RigSpec_requiredBones_get(this));
        },
        undefined);

    // ---- bro.rigging.VoxelChunk ----------------------------------------------
    function VoxelChunk(dimX, dimY, dimZ) {
        return new __bro_native.rigging.VoxelChunk(dimX, dimY, dimZ);
    }
    {
        const proto = __bro_native.rigging.VoxelChunkProto;
        if (proto === undefined) throw new Error("bro.rigging.VoxelChunk: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, VoxelChunk.prototype);
    }
    fn(mount(bro, "rigging"), "VoxelChunk", VoxelChunk);
    fn(VoxelChunk.prototype, "set", function set(x, y, z, value) {
        if (x === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.set: x is required");
        if (y === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.set: y is required");
        if (z === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.set: z is required");
        if (value === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.set: value is required");
        __bro_native.rigging.VoxelChunk_set(this, x, y, z, value);
    });
    fn(VoxelChunk.prototype, "get", function get(x, y, z) {
        if (x === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.get: x is required");
        if (y === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.get: y is required");
        if (z === undefined) throw new TypeError("bro.rigging.VoxelChunk.prototype.get: z is required");
        return __bro_native.rigging.VoxelChunk_get(this, x, y, z);
    });
    fn(VoxelChunk.prototype, "toMesh", function toMesh() {
        return __bro_native.rigging.VoxelChunk_toMesh(this);
    });

    // ---- bro.rigging.IK ------------------------------------------------------
    function IK() {
        throw new TypeError("bro.rigging.IK is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.rigging.IKProto;
        if (proto === undefined) throw new Error("bro.rigging.IK: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, IK.prototype);
    }
    fn(mount(bro, "rigging"), "IK", IK);
    fn(IK, "solveTwoBone", function solveTwoBone(opts) {
        if (opts === undefined) throw new TypeError("bro.rigging.IK.solveTwoBone: opts is required");
        const d_opts = opts;
        return JSON.parse(__bro_native.rigging.IK_solveTwoBone(d_opts.rootPos === undefined ? EMPTY_F64 : toF64(d_opts.rootPos), d_opts.midPos === undefined ? EMPTY_F64 : toF64(d_opts.midPos), d_opts.endPos === undefined ? EMPTY_F64 : toF64(d_opts.endPos), d_opts.targetPos === undefined ? EMPTY_F64 : toF64(d_opts.targetPos), d_opts.poleVector === undefined ? EMPTY_F64 : toF64(d_opts.poleVector), d_opts.length1 !== undefined, d_opts.length1 === undefined ? 0 : d_opts.length1, d_opts.length2 !== undefined, d_opts.length2 === undefined ? 0 : d_opts.length2));
    });
    fn(IK, "solveFabrik", function solveFabrik(opts) {
        if (opts === undefined) throw new TypeError("bro.rigging.IK.solveFabrik: opts is required");
        const d_opts = opts;
        return JSON.parse(__bro_native.rigging.IK_solveFabrik(d_opts.jointPositions === undefined ? '' : JSON.stringify(d_opts.jointPositions), d_opts.targetPos === undefined ? EMPTY_F64 : toF64(d_opts.targetPos), d_opts.tolerance !== undefined, d_opts.tolerance === undefined ? 0 : d_opts.tolerance, d_opts.maxIterations !== undefined, d_opts.maxIterations === undefined ? 0 : d_opts.maxIterations));
    });
    fn(IK, "solveLookAt", function solveLookAt(opts) {
        if (opts === undefined) throw new TypeError("bro.rigging.IK.solveLookAt: opts is required");
        const d_opts = opts;
        return Array.from(__bro_native.rigging.IK_solveLookAt(d_opts.headPos === undefined ? EMPTY_F64 : toF64(d_opts.headPos), d_opts.targetPos === undefined ? EMPTY_F64 : toF64(d_opts.targetPos), d_opts.forward === undefined ? EMPTY_F64 : toF64(d_opts.forward), d_opts.up === undefined ? EMPTY_F64 : toF64(d_opts.up), d_opts.maxAngle !== undefined, d_opts.maxAngle === undefined ? 0 : d_opts.maxAngle));
    });

    // ---- bro.rigging.Rig -----------------------------------------------------
    function Rig() {
        throw new TypeError("bro.rigging.Rig is not constructible: instances come from the natives that return one");
    }
    {
        const proto = __bro_native.rigging.RigProto;
        if (proto === undefined) throw new Error("bro.rigging.Rig: native class prototype not published (registerNatives_rigging did not run)");
        Object.setPrototypeOf(proto, Rig.prototype);
    }
    fn(mount(bro, "rigging"), "Rig", Rig);
    fn(Rig, "detectLandmarks", function detectLandmarks(mesh) {
        if (mesh === undefined) throw new TypeError("bro.rigging.Rig.detectLandmarks: mesh is required");
        __bro_native.rigging.Rig_detectLandmarks(mesh);
        return { landmarks: JSON.parse(__bro_native.rigging.Rig_detectLandmarks_landmarks()), detectedType: __bro_native.rigging.Rig_detectLandmarks_detectedType() };
    });
    fn(Rig, "fitSkeleton", function fitSkeleton(mesh, spec) {
        if (mesh === undefined) throw new TypeError("bro.rigging.Rig.fitSkeleton: mesh is required");
        if (spec === undefined) throw new TypeError("bro.rigging.Rig.fitSkeleton: spec is required");
        __bro_native.rigging.Rig_fitSkeleton(mesh, spec);
        return { skeleton: __bro_native.rigging.Rig_fitSkeleton_skeleton(), skin: __bro_native.rigging.Rig_fitSkeleton_skin() };
    });
    fn(Rig, "autoRig", function autoRig(mesh, opts) {
        if (mesh === undefined) throw new TypeError("bro.rigging.Rig.autoRig: mesh is required");
        const d_opts = opts === undefined ? {} : opts;
        __bro_native.rigging.Rig_autoRig(mesh, d_opts.rigType !== undefined, d_opts.rigType === undefined ? '' : d_opts.rigType, d_opts.maxBones !== undefined, d_opts.maxBones === undefined ? 0 : d_opts.maxBones, d_opts.voxelize !== undefined, d_opts.voxelize === undefined ? false : d_opts.voxelize, d_opts.voxelResolution !== undefined, d_opts.voxelResolution === undefined ? 0 : d_opts.voxelResolution);
        return { skeleton: __bro_native.rigging.Rig_autoRig_skeleton(), skin: __bro_native.rigging.Rig_autoRig_skin() };
    });
    fn(Rig, "transferWeights", function transferWeights(sourceMesh, sourceSkin, targetMesh) {
        if (sourceMesh === undefined) throw new TypeError("bro.rigging.Rig.transferWeights: sourceMesh is required");
        if (sourceSkin === undefined) throw new TypeError("bro.rigging.Rig.transferWeights: sourceSkin is required");
        if (targetMesh === undefined) throw new TypeError("bro.rigging.Rig.transferWeights: targetMesh is required");
        return __bro_native.rigging.Rig_transferWeights(sourceMesh, sourceSkin, targetMesh);
    });

    globalThis.SkinData = SkinData;
    globalThis.Skeleton = Skeleton;
    globalThis.Pose = Pose;
    globalThis.RigSpec = RigSpec;
    globalThis.VoxelChunk = VoxelChunk;
    globalThis.IK = IK;
    globalThis.Rig = Rig;
})();
