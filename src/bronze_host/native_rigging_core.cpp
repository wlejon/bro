#include "native_rigging_internal.h"
#include "json.hpp"

#if BRO_WITH_3D

namespace bro::bronze_host {

using json = nlohmann::json;

namespace {

// --- SkinData ---------------------------------------------------------------

void skinDelete(void* self) {
    delete static_cast<bromesh::SkinData*>(self);
}

void* skinNew(const float* weights, uint32_t weights_len,
             const uint32_t* indices, uint32_t indices_len,
             const float* ibm, uint32_t ibm_len,
             int32_t boneCount) {
    auto* sd = new bromesh::SkinData();
    if (weights && weights_len > 0) sd->boneWeights.assign(weights, weights + weights_len);
    if (indices && indices_len > 0) sd->boneIndices.assign(indices, indices + indices_len);
    if (ibm && ibm_len > 0) sd->inverseBindMatrices.assign(ibm, ibm + ibm_len);
    if (boneCount >= 0) {
        sd->boneCount = static_cast<size_t>(boneCount);
    } else if (!sd->inverseBindMatrices.empty()) {
        sd->boneCount = sd->inverseBindMatrices.size() / 16;
    } else {
        uint32_t maxIdx = 0;
        for (auto idx : sd->boneIndices) {
            if (idx > maxIdx) maxIdx = idx;
        }
        sd->boneCount = sd->boneIndices.empty() ? 0 : static_cast<size_t>(maxIdx + 1);
    }
    return sd;
}

void skinWeights(void* self, bronze_native_buffer* out) {
    copyOut(S(self).boneWeights, out);
}

void skinIndices(void* self, bronze_native_buffer* out) {
    copyOut(S(self).boneIndices, out);
}

void skinInverseBindMatrices(void* self, bronze_native_buffer* out) {
    copyOut(S(self).inverseBindMatrices, out);
}

double skinBoneCount(void* self) {
    return static_cast<double>(S(self).boneCount);
}

double skinVertexCount(void* self) {
    return static_cast<double>(S(self).boneWeights.size() / 4);
}

void skinNormalize(void* self) {
    bromesh::normalizeWeights(S(self));
}

void* skinClone(void* self) {
    return new bromesh::SkinData(S(self));
}

const char* skinValidate(void* mesh, void* skin, int32_t influences, double sumTol) {
    auto res = bromesh::validateSkin(M(mesh), S(skin), influences > 0 ? influences : 4,
                                     sumTol > 0 ? static_cast<float>(sumTol) : 1e-3f);
    json j;
    j["vertexCount"] = res.vertexCount;
    j["orphanCount"] = res.orphanCount;
    j["badSumCount"] = res.badSumCount;
    j["nanCount"] = res.nanCount;
    j["maxSumDeviation"] = res.maxSumDeviation;
    j["maxInfluencesObserved"] = res.maxInfluencesObserved;
    j["clean"] = res.clean();
    return natives::strResult(j.dump());
}

void* skinTransfer(void* targetMesh, void* sourceMesh, void* sourceSkin, double maxDistance) {
    auto out = bromesh::transferSkinWeights(M(targetMesh), M(sourceMesh), S(sourceSkin),
                                            static_cast<float>(maxDistance));
    return new bromesh::SkinData(std::move(out));
}

// --- Skeleton ---------------------------------------------------------------

void skeletonDelete(void* self) {
    delete static_cast<bromesh::Skeleton*>(self);
}

void* skeletonNew(const char* jsonText) {
    auto* skel = new bromesh::Skeleton();
    if (jsonText && *jsonText) {
        try {
            auto j = json::parse(jsonText);
            if (j.contains("bones") && j["bones"].is_array()) {
                for (const auto& bj : j["bones"]) {
                    bromesh::Bone b;
                    b.name = bj.value("name", "");
                    b.parent = bj.value("parent", -1);
                    if (bj.contains("localT") && bj["localT"].is_array()) {
                        for (size_t i = 0; i < std::min<size_t>(3, bj["localT"].size()); ++i)
                            b.localT[i] = bj["localT"][i].get<float>();
                    }
                    if (bj.contains("localR") && bj["localR"].is_array()) {
                        for (size_t i = 0; i < std::min<size_t>(4, bj["localR"].size()); ++i)
                            b.localR[i] = bj["localR"][i].get<float>();
                    }
                    if (bj.contains("localS") && bj["localS"].is_array()) {
                        for (size_t i = 0; i < std::min<size_t>(3, bj["localS"].size()); ++i)
                            b.localS[i] = bj["localS"][i].get<float>();
                    }
                    if (bj.contains("inverseBind") && bj["inverseBind"].is_array()) {
                        for (size_t i = 0; i < std::min<size_t>(16, bj["inverseBind"].size()); ++i)
                            b.inverseBind[i] = bj["inverseBind"][i].get<float>();
                    }
                    skel->bones.push_back(std::move(b));
                }
            }
            if (j.contains("sockets") && j["sockets"].is_array()) {
                for (const auto& sj : j["sockets"]) {
                    bromesh::Socket sock;
                    sock.name = sj.value("name", "");
                    sock.bone = sj.value("bone", 0);
                    if (sj.contains("offset") && sj["offset"].is_array()) {
                        for (size_t i = 0; i < std::min<size_t>(16, sj["offset"].size()); ++i)
                            sock.offset[i] = sj["offset"][i].get<float>();
                    }
                    skel->sockets.push_back(std::move(sock));
                }
            }
        } catch (...) {
            // Ignored, empty skeleton returned
        }
    }
    return skel;
}

double skeletonBoneCount(void* self) {
    return static_cast<double>(K(self).bones.size());
}

double skeletonSocketCount(void* self) {
    return static_cast<double>(K(self).sockets.size());
}

const char* skeletonBonesJSON(void* self) {
    json arr = json::array();
    for (const auto& b : K(self).bones) {
        json bj;
        bj["name"] = b.name;
        bj["parent"] = b.parent;
        bj["localT"] = { b.localT[0], b.localT[1], b.localT[2] };
        bj["localR"] = { b.localR[0], b.localR[1], b.localR[2], b.localR[3] };
        bj["localS"] = { b.localS[0], b.localS[1], b.localS[2] };
        bj["inverseBind"] = std::vector<float>(b.inverseBind, b.inverseBind + 16);
        arr.push_back(bj);
    }
    return natives::strResult(arr.dump());
}

const char* skeletonSocketsJSON(void* self) {
    json arr = json::array();
    for (const auto& s : K(self).sockets) {
        json sj;
        sj["name"] = s.name;
        sj["bone"] = s.bone;
        sj["offset"] = std::vector<float>(s.offset, s.offset + 16);
        arr.push_back(sj);
    }
    return natives::strResult(arr.dump());
}

double skeletonFindBone(void* self, const char* name) {
    return static_cast<double>(K(self).findBone(name ? name : ""));
}

double skeletonFindSocket(void* self, const char* name) {
    return static_cast<double>(K(self).findSocket(name ? name : ""));
}

double skeletonAddSocket(void* self, const char* name, int32_t bone, const float* offset, uint32_t offset_len) {
    bromesh::Socket sock;
    sock.name = name ? name : "";
    sock.bone = bone;
    if (offset && offset_len >= 16) {
        std::memcpy(sock.offset, offset, 16 * sizeof(float));
    }
    K(self).sockets.push_back(std::move(sock));
    return static_cast<double>(K(self).sockets.size() - 1);
}

void* skeletonBindPose(void* self) {
    return new bromesh::Pose(bromesh::bindPose(K(self)));
}

void* skeletonClone(void* self) {
    return new bromesh::Skeleton(K(self));
}

double skeletonFindBoneBySuffix(void* self, const char* suffix) {
    if (!self || !suffix) return -1.0;
    return static_cast<double>(bromesh::findBoneBySuffix(K(self), suffix));
}

// --- Pose -------------------------------------------------------------------

void poseDelete(void* self) {
    delete static_cast<bromesh::Pose*>(self);
}

void* poseNew(const float* data, uint32_t data_len, int32_t boneCount) {
    auto* p = new bromesh::Pose();
    if (data && data_len > 0) {
        p->data.assign(data, data + data_len);
    } else if (boneCount > 0) {
        p->data.assign(static_cast<size_t>(boneCount) * 10, 0.0f);
        for (int32_t i = 0; i < boneCount; ++i) {
            p->data[i * 10 + 6] = 1.0f; // qw
            p->data[i * 10 + 7] = 1.0f; // sx
            p->data[i * 10 + 8] = 1.0f; // sy
            p->data[i * 10 + 9] = 1.0f; // sz
        }
    }
    return p;
}

void poseDataGet(void* self, bronze_native_buffer* out) {
    copyOut(P(self).data, out);
}

void poseDataSet(void* self, const float* data, uint32_t data_len) {
    if (data && data_len > 0) P(self).data.assign(data, data + data_len);
    else P(self).data.clear();
}

double poseBoneCount(void* self) {
    return static_cast<double>(P(self).boneCount());
}

void poseComputeWorldMatrices(void* self, void* skeleton, bronze_native_buffer* out) {
    static thread_local std::vector<float> tl_worldMats;
    bromesh::computeWorldMatrices(K(skeleton), P(self), tl_worldMats);
    copyOut(tl_worldMats, out);
}

void poseComputeSkinningMatrices(void* self, void* skeleton, bronze_native_buffer* out) {
    static thread_local std::vector<float> tl_skinMats;
    bromesh::computeSkinningMatrices(K(skeleton), P(self), tl_skinMats);
    copyOut(tl_skinMats, out);
}

void poseSocketWorld(void* self, void* skeleton, const char* name, bronze_native_buffer* out) {
    static thread_local std::vector<float> tl_sockMat;
    auto res = bromesh::socketWorldMatrix(K(skeleton), P(self), name ? name : "");
    if (res) {
        tl_sockMat.assign(res->begin(), res->end());
        copyOut(tl_sockMat, out);
    } else {
        tl_sockMat.clear();
        copyOut(tl_sockMat, out);
    }
}

void* poseClone(void* self) {
    return new bromesh::Pose(P(self));
}

void poseBlend(void* a, void* b, double weight) {
    bromesh::blendPoses(P(a), P(b), static_cast<float>(weight));
}

// --- VoxelChunk -------------------------------------------------------------

void voxelChunkDelete(void* self) {
    delete static_cast<bromesh::VoxelChunk*>(self);
}

void* voxelChunkNew(int32_t sx, int32_t sy, int32_t sz, double cellSize) {
    return new bromesh::VoxelChunk(sx > 0 ? sx : 1, sy > 0 ? sy : 1, sz > 0 ? sz : 1,
                                   cellSize > 0 ? static_cast<float>(cellSize) : 1.0f);
}

double voxelChunkSizeX(void* self) { return static_cast<double>(V(self).sizeX()); }
double voxelChunkSizeY(void* self) { return static_cast<double>(V(self).sizeY()); }
double voxelChunkSizeZ(void* self) { return static_cast<double>(V(self).sizeZ()); }
double voxelChunkCellSize(void* self) { return static_cast<double>(V(self).cellSize()); }
bool voxelChunkIsDirty(void* self) { return V(self).isDirty(); }
void voxelChunkSetIsDirty(void* self, bool d) { if (d) V(self).markDirty(); else V(self).clearDirty(); }

void voxelChunkSetVoxel(void* self, int32_t x, int32_t y, int32_t z, int32_t val) {
    V(self).setVoxel(x, y, z, static_cast<uint8_t>(val));
}

double voxelChunkGetVoxel(void* self, int32_t x, int32_t y, int32_t z) {
    return static_cast<double>(V(self).getVoxel(x, y, z));
}

void voxelChunkFill(void* self, int32_t val) {
    V(self).fill(static_cast<uint8_t>(val));
}

void voxelChunkMarkDirty(void* self) { V(self).markDirty(); }
void voxelChunkClearDirty(void* self) { V(self).clearDirty(); }

void voxelChunkData(void* self, bronze_native_buffer* out) {
    size_t n = static_cast<size_t>(V(self).sizeX()) * V(self).sizeY() * V(self).sizeZ();
    out->data = V(self).data();
    out->length = static_cast<uint32_t>(n);
    out->release = nullptr;
}

void voxelChunkSetData(void* self, const uint8_t* data, uint32_t len) {
    size_t n = static_cast<size_t>(V(self).sizeX()) * V(self).sizeY() * V(self).sizeZ();
    size_t copyN = std::min(static_cast<size_t>(len), n);
    if (data && copyN > 0) {
        std::memcpy(V(self).data(), data, copyN);
        V(self).markDirty();
    }
}

void* voxelChunkBuildMesh(void* self, const float* pal, uint32_t pal_len, int32_t palCount) {
    int count = palCount > 0 ? palCount : static_cast<int>(pal_len / 4);
    auto mesh = V(self).buildMesh(pal && count > 0 ? pal : nullptr, count);
    return new bromesh::MeshData(std::move(mesh));
}

}  // namespace

bool registerRiggingCoreNatives(std::string* error) {
    const bool ok =
        // Class constructors first so types are known to subsequent method registrations
        ctor(kSkinData, p(&skinNew), &skinDelete, bronze::runtime::Finalize::InSweep,
             {"f32[]", "u32[]", "f32[]", "i32"}, error) &&
        ctor(kSkeleton, p(&skeletonNew), &skeletonDelete, bronze::runtime::Finalize::InSweep,
             {"str"}, error) &&
        ctor(kPose, p(&poseNew), &poseDelete, bronze::runtime::Finalize::InSweep,
             {"f32[]", "i32"}, error) &&
        ctor(kVoxelChunk, p(&voxelChunkNew), &voxelChunkDelete, bronze::runtime::Finalize::InSweep,
             {"i32", "i32", "i32", "f64"}, error) &&

        // SkinData methods
        fn("__bro_native.rigging.SkinData_boneWeights", p(&skinWeights), "f32[]", {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_boneIndices", p(&skinIndices), "u32[]", {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_inverseBindMatrices", p(&skinInverseBindMatrices), "f32[]", {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_boneCount_get", p(&skinBoneCount), "f64", {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_vertexCount_get", p(&skinVertexCount), "f64", {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_normalize", p(&skinNormalize), "void", {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_clone", p(&skinClone), kSkinData, {kSkinData}, error) &&
        fn("__bro_native.rigging.SkinData_validate", p(&skinValidate), "str", {kMesh, kSkinData, "i32", "f64"}, error) &&
        fn("__bro_native.rigging.SkinData_transfer", p(&skinTransfer), kSkinData, {kMesh, kMesh, kSkinData, "f64"}, error) &&

        // Skeleton methods
        fn("__bro_native.rigging.Skeleton_boneCount_get", p(&skeletonBoneCount), "f64", {kSkeleton}, error) &&
        fn("__bro_native.rigging.Skeleton_socketCount_get", p(&skeletonSocketCount), "f64", {kSkeleton}, error) &&
        fn("__bro_native.rigging.Skeleton_bonesJSON", p(&skeletonBonesJSON), "str", {kSkeleton}, error) &&
        fn("__bro_native.rigging.Skeleton_socketsJSON", p(&skeletonSocketsJSON), "str", {kSkeleton}, error) &&
        fn("__bro_native.rigging.Skeleton_findBone", p(&skeletonFindBone), "f64", {kSkeleton, "str"}, error) &&
        fn("__bro_native.rigging.Skeleton_findBoneBySuffix", p(&skeletonFindBoneBySuffix), "f64", {kSkeleton, "str"}, error) &&
        fn("__bro_native.rigging.Skeleton_findSocket", p(&skeletonFindSocket), "f64", {kSkeleton, "str"}, error) &&
        fn("__bro_native.rigging.Skeleton_addSocket", p(&skeletonAddSocket), "f64", {kSkeleton, "str", "i32", "f32[]"}, error) &&
        fn("__bro_native.rigging.Skeleton_bindPose", p(&skeletonBindPose), kPose, {kSkeleton}, error) &&
        fn("__bro_native.rigging.Skeleton_clone", p(&skeletonClone), kSkeleton, {kSkeleton}, error) &&

        // Pose methods
        fn("__bro_native.rigging.Pose_data_get", p(&poseDataGet), "f32[]", {kPose}, error) &&
        fn("__bro_native.rigging.Pose_data_set", p(&poseDataSet), "void", {kPose, "f32[]"}, error) &&
        fn("__bro_native.rigging.Pose_boneCount_get", p(&poseBoneCount), "f64", {kPose}, error) &&
        fn("__bro_native.rigging.Pose_computeWorldMatrices", p(&poseComputeWorldMatrices), "f32[]", {kPose, kSkeleton}, error) &&
        fn("__bro_native.rigging.Pose_computeSkinningMatrices", p(&poseComputeSkinningMatrices), "f32[]", {kPose, kSkeleton}, error) &&
        fn("__bro_native.rigging.Pose_socketWorld", p(&poseSocketWorld), "f32[]", {kPose, kSkeleton, "str"}, error) &&
        fn("__bro_native.rigging.Pose_clone", p(&poseClone), kPose, {kPose}, error) &&
        fn("__bro_native.rigging.Pose_blend", p(&poseBlend), "void", {kPose, kPose, "f64"}, error) &&

        // VoxelChunk methods
        fn("__bro_native.rigging.VoxelChunk_sizeX_get", p(&voxelChunkSizeX), "f64", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_sizeY_get", p(&voxelChunkSizeY), "f64", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_sizeZ_get", p(&voxelChunkSizeZ), "f64", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_cellSize_get", p(&voxelChunkCellSize), "f64", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_isDirty_get", p(&voxelChunkIsDirty), "bool", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_isDirty_set", p(&voxelChunkSetIsDirty), "void", {kVoxelChunk, "bool"}, error) &&
        fn("__bro_native.rigging.VoxelChunk_setVoxel", p(&voxelChunkSetVoxel), "void", {kVoxelChunk, "i32", "i32", "i32", "i32"}, error) &&
        fn("__bro_native.rigging.VoxelChunk_getVoxel", p(&voxelChunkGetVoxel), "f64", {kVoxelChunk, "i32", "i32", "i32"}, error) &&
        fn("__bro_native.rigging.VoxelChunk_fill", p(&voxelChunkFill), "void", {kVoxelChunk, "i32"}, error) &&
        fn("__bro_native.rigging.VoxelChunk_markDirty", p(&voxelChunkMarkDirty), "void", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_clearDirty", p(&voxelChunkClearDirty), "void", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_data", p(&voxelChunkData), "u8[]", {kVoxelChunk}, error) &&
        fn("__bro_native.rigging.VoxelChunk_setData", p(&voxelChunkSetData), "void", {kVoxelChunk, "u8[]"}, error) &&
        fn("__bro_native.rigging.VoxelChunk_buildMesh", p(&voxelChunkBuildMesh), kMesh, {kVoxelChunk, "f32[]", "i32"}, error);

    return ok;
}

}  // namespace bro::bronze_host

#endif
