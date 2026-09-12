#pragma once

#if BRO_WITH_3D

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bromesh/mesh_data.h"
#include "bromesh/manipulation/skin.h"
#include "bromesh/manipulation/skin_transfer.h"
#include "bromesh/rigging/skin_validate.h"
#include "bromesh/animation/pose.h"
#include "bromesh/animation/retarget.h"
#include "bromesh/animation/ik.h"
#include "bromesh/animation/locomotion.h"
#include "bromesh/rigging/rig_spec.h"
#include "bromesh/rigging/landmarks.h"
#include "bromesh/rigging/landmark_detect.h"
#include "bromesh/rigging/skeleton_fit.h"
#include "bromesh/rigging/auto_rig.h"
#include "bromesh/rigging/weighting.h"
#include "bromesh/voxel/voxel_chunk.h"

#include <memory>
#include <string>
#include <vector>
#include <span>

namespace bro::bronze_host {

inline constexpr uint32_t kHostSkinDataTag   = 0x534B494Eu;  // 'SKIN'
inline constexpr uint32_t kHostSkeletonTag   = 0x534B454Cu;  // 'SKEL'
inline constexpr uint32_t kHostPoseTag       = 0x504F5345u;  // 'POSE'
inline constexpr uint32_t kHostAnimationTag  = 0x414E494Du;  // 'ANIM'
inline constexpr uint32_t kHostRigSpecTag    = 0x52535043u;  // 'RSPC'
inline constexpr uint32_t kHostVoxelChunkTag = 0x56584348u;  // 'VXCH'

struct HostSkinData {
    uint32_t tag = kHostSkinDataTag;
    std::unique_ptr<bromesh::SkinData> data;
};

struct HostSkeleton {
    uint32_t tag = kHostSkeletonTag;
    std::unique_ptr<bromesh::Skeleton> skel;
};

struct HostPose {
    uint32_t tag = kHostPoseTag;
    std::unique_ptr<bromesh::Pose> pose;
};

struct HostAnimation {
    uint32_t tag = kHostAnimationTag;
    std::unique_ptr<bromesh::Animation> anim;
};

struct HostRigSpec {
    uint32_t tag = kHostRigSpecTag;
    std::unique_ptr<bromesh::RigSpec> spec;
};

struct HostVoxelChunk {
    uint32_t tag = kHostVoxelChunkTag;
    std::unique_ptr<bromesh::VoxelChunk> chunk;
};

extern HostClass g_skinDataClass;
extern HostClass g_skeletonClass;
extern HostClass g_poseClass;
extern HostClass g_animationClass;
extern HostClass g_rigSpecClass;
extern HostClass g_voxelChunkClass;

// Unwrappers
inline HostSkinData* hostSkinDataCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostSkinData*>(ev::handleData(v));
    return (p && p->tag == kHostSkinDataTag) ? p : nullptr;
}

inline bromesh::SkinData* hostSkinDataOf(Value v) {
    auto* c = hostSkinDataCellOf(v);
    return (c && c->data) ? c->data.get() : nullptr;
}

inline HostSkeleton* hostSkeletonCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostSkeleton*>(ev::handleData(v));
    return (p && p->tag == kHostSkeletonTag) ? p : nullptr;
}

inline bromesh::Skeleton* hostSkeletonOf(Value v) {
    auto* c = hostSkeletonCellOf(v);
    return (c && c->skel) ? c->skel.get() : nullptr;
}

inline HostPose* hostPoseCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostPose*>(ev::handleData(v));
    return (p && p->tag == kHostPoseTag) ? p : nullptr;
}

inline bromesh::Pose* hostPoseOf(Value v) {
    auto* c = hostPoseCellOf(v);
    return (c && c->pose) ? c->pose.get() : nullptr;
}

inline HostAnimation* hostAnimationCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAnimation*>(ev::handleData(v));
    return (p && p->tag == kHostAnimationTag) ? p : nullptr;
}

inline bromesh::Animation* hostAnimationOf(Value v) {
    auto* c = hostAnimationCellOf(v);
    return (c && c->anim) ? c->anim.get() : nullptr;
}

inline HostRigSpec* hostRigSpecCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostRigSpec*>(ev::handleData(v));
    return (p && p->tag == kHostRigSpecTag) ? p : nullptr;
}

inline bromesh::RigSpec* hostRigSpecOf(Value v) {
    auto* c = hostRigSpecCellOf(v);
    return (c && c->spec) ? c->spec.get() : nullptr;
}

inline HostVoxelChunk* hostVoxelChunkCellOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostVoxelChunk*>(ev::handleData(v));
    return (p && p->tag == kHostVoxelChunkTag) ? p : nullptr;
}

inline bromesh::VoxelChunk* hostVoxelChunkOf(Value v) {
    auto* c = hostVoxelChunkCellOf(v);
    return (c && c->chunk) ? c->chunk.get() : nullptr;
}

// Wrappers
Value wrapSkinData(std::unique_ptr<bromesh::SkinData> data);
Value wrapSkinData(bromesh::SkinData&& data);
Value wrapSkeleton(std::unique_ptr<bromesh::Skeleton> skel);
Value wrapSkeleton(bromesh::Skeleton&& skel);
Value wrapPose(std::unique_ptr<bromesh::Pose> pose);
Value wrapPose(bromesh::Pose&& pose);
Value wrapAnimation(std::unique_ptr<bromesh::Animation> anim);
Value wrapAnimation(bromesh::Animation&& anim);
Value wrapRigSpec(std::unique_ptr<bromesh::RigSpec> spec);
Value wrapRigSpec(bromesh::RigSpec&& spec);
Value wrapVoxelChunk(std::unique_ptr<bromesh::VoxelChunk> chunk);

// Serialization helpers
inline void readFloatN(Value v, float* out, size_t n, const float* defVals) {
    if (defVals) {
        for (size_t i = 0; i < n; ++i) out[i] = defVals[i];
    }
    if (auto info = ev::typedArrayInfo(v)) {
        if (info.data) {
            const float* p = reinterpret_cast<const float*>(info.data);
            size_t count = std::min(n, info.byteLength / sizeof(float));
            for (size_t i = 0; i < count; ++i) out[i] = p[i];
            return;
        }
    }
    if (ev::isObject(v)) {
        for (size_t i = 0; i < n; ++i) {
            Value el = ev::getElement(v, static_cast<uint32_t>(i));
            if (ev::isNumber(el)) out[i] = static_cast<float>(ev::toDouble(el));
        }
    }
}

inline Value makeFloatNArray(const float* data, size_t n) {
    return makeFloat32Array(data, n);
}

inline bromesh::Bone readBone(Value obj) {
    bromesh::Bone b;
    if (!ev::isObject(obj)) return b;

    Value nameV = ev::getProperty(obj, "name");
    if (ev::isString(nameV)) b.name = ev::toUtf8(nameV);

    Value parentV = ev::getProperty(obj, "parent");
    if (ev::isNumber(parentV)) b.parent = static_cast<int32_t>(ev::toDouble(parentV));

    static const float defT[3] = {0,0,0};
    static const float defR[4] = {0,0,0,1};
    static const float defS[3] = {1,1,1};
    static const float defIB[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    readFloatN(ev::getProperty(obj, "localT"), b.localT, 3, defT);
    readFloatN(ev::getProperty(obj, "localR"), b.localR, 4, defR);
    readFloatN(ev::getProperty(obj, "localS"), b.localS, 3, defS);
    readFloatN(ev::getProperty(obj, "inverseBind"), b.inverseBind, 16, defIB);

    return b;
}

inline Value makeBone(const bromesh::Bone& b) {
    ev::Persistent root(ev::createObject());
    ev::setProperty(root.get(), "name", ev::fromUtf8(b.name));
    ev::setProperty(root.get(), "parent", ev::fromDouble(b.parent));
    ev::setProperty(root.get(), "localT", makeFloatNArray(b.localT, 3));
    ev::setProperty(root.get(), "localR", makeFloatNArray(b.localR, 4));
    ev::setProperty(root.get(), "localS", makeFloatNArray(b.localS, 3));
    ev::setProperty(root.get(), "inverseBind", makeFloatNArray(b.inverseBind, 16));
    return root.get();
}

inline void readBonesArray(Value arr, std::vector<bromesh::Bone>& out) {
    out.clear();
    if (!ev::isObject(arr)) return;
    Value lenV = ev::getProperty(arr, "length");
    if (!ev::isNumber(lenV)) return;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        out.push_back(readBone(ev::getElement(arr, i)));
    }
}

inline Value makeBonesArray(const std::vector<bromesh::Bone>& bones) {
    return hostArrayOf(bones.size(), [&](size_t i) -> Value {
        return makeBone(bones[i]);
    });
}

inline bromesh::Socket readSocket(Value obj) {
    bromesh::Socket s;
    if (!ev::isObject(obj)) return s;

    Value nameV = ev::getProperty(obj, "name");
    if (ev::isString(nameV)) s.name = ev::toUtf8(nameV);

    Value boneV = ev::getProperty(obj, "bone");
    if (ev::isNumber(boneV)) s.bone = static_cast<int32_t>(ev::toDouble(boneV));

    static const float defOffset[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    readFloatN(ev::getProperty(obj, "offset"), s.offset, 16, defOffset);

    return s;
}

inline Value makeSocket(const bromesh::Socket& s) {
    ev::Persistent root(ev::createObject());
    ev::setProperty(root.get(), "name", ev::fromUtf8(s.name));
    ev::setProperty(root.get(), "bone", ev::fromDouble(s.bone));
    ev::setProperty(root.get(), "offset", makeFloatNArray(s.offset, 16));
    return root.get();
}

inline void readSocketsArray(Value arr, std::vector<bromesh::Socket>& out) {
    out.clear();
    if (!ev::isObject(arr)) return;
    Value lenV = ev::getProperty(arr, "length");
    if (!ev::isNumber(lenV)) return;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        out.push_back(readSocket(ev::getElement(arr, i)));
    }
}

inline Value makeSocketsArray(const std::vector<bromesh::Socket>& sks) {
    return hostArrayOf(sks.size(), [&](size_t i) -> Value {
        return makeSocket(sks[i]);
    });
}

inline Value makeFloatArray(const std::vector<float>& vec) {
    return makeFloat32Array(vec);
}

inline Value makeStringArray(const std::vector<std::string>& vec) {
    return hostArrayOf(vec.size(), [&](size_t i) -> Value {
        return ev::fromUtf8(vec[i]);
    });
}

inline bromesh::Landmarks readLandmarks(Value obj) {
    bromesh::Landmarks lm;
    if (!ev::isObject(obj)) return lm;
    auto objCtor = ev::globalValue("Object");
    if (!objCtor.found) return lm;
    ev::Persistent objCtorP(objCtor.value);
    Value keysFn = ev::getProperty(objCtorP.get(), "keys");
    if (!ev::isFunction(keysFn)) return lm;
    ev::CallResult r = ev::call(keysFn, objCtorP.get(), std::span<const Value>(&obj, 1));
    if (r.thrown || !ev::isObject(r.value)) return lm;
    ev::Persistent keysRoot(r.value);
    Value lenV = ev::getProperty(keysRoot.get(), "length");
    if (!ev::isNumber(lenV)) return lm;
    uint32_t len = static_cast<uint32_t>(ev::toDouble(lenV));
    for (uint32_t i = 0; i < len; ++i) {
        Value kVal = ev::getElement(keysRoot.get(), i);
        if (ev::isString(kVal)) {
            std::string k = ev::toUtf8(kVal);
            Value v = ev::getProperty(obj, k.c_str());
            float p[3] = {0,0,0};
            readFloatN(v, p, 3, nullptr);
            lm.set(k, p[0], p[1], p[2]);
        }
    }
    return lm;
}

inline Value makeLandmarks(const bromesh::Landmarks& lm) {
    ev::Persistent root(ev::createObject());
    for (const auto& [name, pos] : lm.points) {
        float p[3] = {pos[0], pos[1], pos[2]};
        ev::setProperty(root.get(), name.c_str(), makeFloatNArray(p, 3));
    }
    return root.get();
}

inline void readWeightingOptions(Value obj, bromesh::WeightingOptions& wo) {
    if (!ev::isObject(obj)) return;

    Value methV = ev::getProperty(obj, "method");
    if (ev::isString(methV)) {
        std::string m = ev::toUtf8(methV);
        wo.method = bromesh::parseWeightingMethod(m.c_str());
    }

    Value smV = ev::getProperty(obj, "smoothIterations");
    if (ev::isNumber(smV)) wo.smoothIterations = static_cast<int>(ev::toDouble(smV));

    Value smAV = ev::getProperty(obj, "smoothAlpha");
    if (ev::isNumber(smAV)) wo.smoothAlpha = static_cast<float>(ev::toDouble(smAV));

    Value minWV = ev::getProperty(obj, "minWeight");
    if (ev::isNumber(minWV)) wo.minWeight = static_cast<float>(ev::toDouble(minWV));

    Value voxV = ev::getProperty(obj, "voxel");
    if (ev::isObject(voxV)) {
        Value mr = ev::getProperty(voxV, "maxResolution");
        if (ev::isNumber(mr)) wo.voxel.maxResolution = static_cast<int>(ev::toDouble(mr));
        Value mi = ev::getProperty(voxV, "maxInfluences");
        if (ev::isNumber(mi)) wo.voxel.maxInfluences = static_cast<int>(ev::toDouble(mi));
        Value fp = ev::getProperty(voxV, "falloffPower");
        if (ev::isNumber(fp)) wo.voxel.falloffPower = static_cast<float>(ev::toDouble(fp));
        Value mw = ev::getProperty(voxV, "minWeight");
        if (ev::isNumber(mw)) wo.voxel.minWeight = static_cast<float>(ev::toDouble(mw));
        Value si = ev::getProperty(voxV, "smoothIterations");
        if (ev::isNumber(si)) wo.voxel.smoothIterations = static_cast<int>(ev::toDouble(si));
        Value sa = ev::getProperty(voxV, "smoothAlpha");
        if (ev::isNumber(sa)) wo.voxel.smoothAlpha = static_cast<float>(ev::toDouble(sa));
    }

    Value bhV = ev::getProperty(obj, "boneHeat");
    if (ev::isObject(bhV)) {
        Value mi = ev::getProperty(bhV, "maxInfluences");
        if (ev::isNumber(mi)) wo.boneHeat.maxInfluences = static_cast<int>(ev::toDouble(mi));
        Value mw = ev::getProperty(bhV, "minWeight");
        if (ev::isNumber(mw)) wo.boneHeat.minWeight = static_cast<float>(ev::toDouble(mw));
        Value hs = ev::getProperty(bhV, "heatStrength");
        if (ev::isNumber(hs)) wo.boneHeat.heatStrength = static_cast<float>(ev::toDouble(hs));
        Value st = ev::getProperty(bhV, "solverTol");
        if (ev::isNumber(st)) wo.boneHeat.solverTol = ev::toDouble(st);
        Value smi = ev::getProperty(bhV, "solverMaxIter");
        if (ev::isNumber(smi)) wo.boneHeat.solverMaxIter = static_cast<int>(ev::toDouble(smi));
    }

    Value bbwV = ev::getProperty(obj, "bbw");
    if (ev::isObject(bbwV)) {
        Value mi = ev::getProperty(bbwV, "maxInfluences");
        if (ev::isNumber(mi)) wo.bbw.maxInfluences = static_cast<int>(ev::toDouble(mi));
        Value mw = ev::getProperty(bbwV, "minWeight");
        if (ev::isNumber(mw)) wo.bbw.minWeight = static_cast<float>(ev::toDouble(mw));
        Value apb = ev::getProperty(bbwV, "anchorsPerBone");
        if (ev::isNumber(apb)) wo.bbw.anchorsPerBone = static_cast<int>(ev::toDouble(apb));
        Value eps = ev::getProperty(bbwV, "eps");
        if (ev::isNumber(eps)) wo.bbw.eps = ev::toDouble(eps);
        Value mi_ = ev::getProperty(bbwV, "maxIter");
        if (ev::isNumber(mi_)) wo.bbw.maxIter = static_cast<int>(ev::toDouble(mi_));
    }
}

void installRiggingGlobals();
void installAnimationClass(HostClass& cls);
Value js_rig_generateLocomotionCycle(std::span<const Value> a);

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
