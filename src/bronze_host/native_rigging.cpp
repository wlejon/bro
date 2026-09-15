// native_rigging.cpp — C entry points behind bro.rigging over bromesh.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "natives/rigging/native_rigging_decl.h"

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/skin.h>
#include <bromesh/manipulation/skin_transfer.h>
#include <bromesh/animation/pose.h>
#include <bromesh/animation/ik.h>
#include <bromesh/rigging/rig_spec.h>
#include <bromesh/rigging/landmarks.h>
#include <bromesh/rigging/auto_rig.h>
#include <bromesh/voxel/voxel_chunk.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

bool registerNatives_rigging(std::string* error);

bool registerRiggingNatives(std::string* error) {
    return registerNatives_rigging(error);
}

namespace {

static thread_local std::vector<double> tl_matScratch;

struct PoseTRSScratch {
    double t[3] = {0,0,0};
    double r[4] = {0,0,0,1};
    double s[3] = {1,1,1};
};
static thread_local PoseTRSScratch tl_poseTRS;

struct RigFitSlot {
    std::unique_ptr<bromesh::Skeleton> skeleton;
    std::unique_ptr<bromesh::SkinData> skin;
};
static thread_local RigFitSlot tl_rigFit;

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

// --- SkinData ---------------------------------------------------------------

void bro_rigging_SkinData_dtor(void* self) {
    delete static_cast<bromesh::SkinData*>(self);
}

void* bro_rigging_SkinData_ctor(bool opts_vertexCount_given, int32_t opts_vertexCount,
                                bool opts_maxWeightsPerVertex_given, int32_t opts_maxWeightsPerVertex,
                                const uint16_t* opts_indices, uint32_t opts_indices_len,
                                const float* opts_weights, uint32_t opts_weights_len) {
    auto* sd = new bromesh::SkinData();
    if (opts_weights && opts_weights_len > 0) {
        sd->boneWeights.assign(opts_weights, opts_weights + opts_weights_len);
    }
    if (opts_indices && opts_indices_len > 0) {
        sd->boneIndices.assign(opts_indices, opts_indices + opts_indices_len);
    }
    if (opts_vertexCount_given && opts_vertexCount > 0) {
        sd->boneCount = static_cast<size_t>(opts_vertexCount);
    }
    return sd;
}

int32_t bro_rigging_SkinData_vertexCount_get(void* self) {
    auto* sd = static_cast<bromesh::SkinData*>(self);
    return sd ? static_cast<int32_t>(sd->boneWeights.size() / 4) : 0;
}

int32_t bro_rigging_SkinData_maxWeights_get(void* self) {
    return 4;
}

void* bro_rigging_SkinData_clone(void* self) {
    auto* sd = static_cast<bromesh::SkinData*>(self);
    return sd ? new bromesh::SkinData(*sd) : nullptr;
}

bool bro_rigging_SkinData_validate(void* self) {
    auto* sd = static_cast<bromesh::SkinData*>(self);
    return sd ? sd->validate() : false;
}

void bro_rigging_SkinData_normalize(void* self) {
    auto* sd = static_cast<bromesh::SkinData*>(self);
    if (sd) bromesh::normalizeWeights(*sd);
}

// --- Skeleton ---------------------------------------------------------------

void bro_rigging_Skeleton_dtor(void* self) {
    delete static_cast<bromesh::Skeleton*>(self);
}

void* bro_rigging_Skeleton_ctor(const char* opts_bones, const char* opts_sockets) {
    auto* skel = new bromesh::Skeleton();
    return skel;
}

int32_t bro_rigging_Skeleton_boneCount_get(void* self) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    return skel ? static_cast<int32_t>(skel->bones.size()) : 0;
}

int32_t bro_rigging_Skeleton_findBone(void* self, const char* name) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    return (skel && name) ? skel->findBone(name) : -1;
}

const char* bro_rigging_Skeleton_boneName(void* self, int32_t index) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    if (!skel || index < 0 || static_cast<size_t>(index) >= skel->bones.size()) return "";
    return natives::strResult(skel->bones[index].name);
}

int32_t bro_rigging_Skeleton_boneParent(void* self, int32_t index) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    if (!skel || index < 0 || static_cast<size_t>(index) >= skel->bones.size()) return -1;
    return skel->bones[index].parent;
}

void bro_rigging_Skeleton_boneBindPose(void* self, int32_t index, bronze_native_buffer* out) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    tl_matScratch.assign(16, 0.0);
    for (int i = 0; i < 4; ++i) tl_matScratch[i * 5] = 1.0;
    out->data = tl_matScratch.data();
    out->length = static_cast<uint32_t>(tl_matScratch.size());
    out->release = nullptr;
}

void bro_rigging_Skeleton_boneInverseBind(void* self, int32_t index, bronze_native_buffer* out) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    tl_matScratch.assign(16, 0.0);
    for (int i = 0; i < 4; ++i) tl_matScratch[i * 5] = 1.0;
    if (skel && index >= 0 && static_cast<size_t>(index) < skel->bones.size()) {
        const auto& b = skel->bones[index];
        for (int i = 0; i < 16; ++i) tl_matScratch[i] = b.inverseBind[i];
    }
    out->data = tl_matScratch.data();
    out->length = static_cast<uint32_t>(tl_matScratch.size());
    out->release = nullptr;
}

void* bro_rigging_Skeleton_clone(void* self) {
    auto* skel = static_cast<bromesh::Skeleton*>(self);
    return skel ? new bromesh::Skeleton(*skel) : nullptr;
}

// --- Pose -------------------------------------------------------------------

void bro_rigging_Pose_dtor(void* self) {
    delete static_cast<bromesh::Pose*>(self);
}

void* bro_rigging_Pose_ctor(void* skeleton) {
    auto* skel = static_cast<bromesh::Skeleton*>(skeleton);
    if (!skel) return new bromesh::Pose();
    return new bromesh::Pose(bromesh::bindPose(*skel));
}

int32_t bro_rigging_Pose_boneCount_get(void* self) {
    auto* p = static_cast<bromesh::Pose*>(self);
    return p ? static_cast<int32_t>(p->boneCount()) : 0;
}

void bro_rigging_Pose_getBoneLocal(void* self, int32_t index) {
    auto* p = static_cast<bromesh::Pose*>(self);
    tl_poseTRS = {};
    if (p && index >= 0 && static_cast<size_t>(index) < p->boneCount()) {
        const float* d = p->data.data() + index * 10;
        tl_poseTRS.t[0] = d[0]; tl_poseTRS.t[1] = d[1]; tl_poseTRS.t[2] = d[2];
        tl_poseTRS.r[0] = d[3]; tl_poseTRS.r[1] = d[4]; tl_poseTRS.r[2] = d[5]; tl_poseTRS.r[3] = d[6];
        tl_poseTRS.s[0] = d[7]; tl_poseTRS.s[1] = d[8]; tl_poseTRS.s[2] = d[9];
    }
}

void bro_rigging_Pose_getBoneLocal_translation(bronze_native_buffer* out) {
    out->data = tl_poseTRS.t;
    out->length = 3;
    out->release = nullptr;
}

void bro_rigging_Pose_getBoneLocal_rotation(bronze_native_buffer* out) {
    out->data = tl_poseTRS.r;
    out->length = 4;
    out->release = nullptr;
}

void bro_rigging_Pose_getBoneLocal_scale(bronze_native_buffer* out) {
    out->data = tl_poseTRS.s;
    out->length = 3;
    out->release = nullptr;
}

void bro_rigging_Pose_setBoneLocal(void* self, int32_t index,
                                   const double* trs_translation, uint32_t trs_translation_len,
                                   const double* trs_rotation, uint32_t trs_rotation_len,
                                   const double* trs_scale, uint32_t trs_scale_len) {
    auto* p = static_cast<bromesh::Pose*>(self);
    if (!p || index < 0) return;
    if (static_cast<size_t>(index) >= p->boneCount()) {
        p->data.resize((index + 1) * 10, 0.0f);
    }
    float* d = p->data.data() + index * 10;
    if (trs_translation && trs_translation_len >= 3) {
        d[0] = (float)trs_translation[0]; d[1] = (float)trs_translation[1]; d[2] = (float)trs_translation[2];
    }
    if (trs_rotation && trs_rotation_len >= 4) {
        d[3] = (float)trs_rotation[0]; d[4] = (float)trs_rotation[1]; d[5] = (float)trs_rotation[2]; d[6] = (float)trs_rotation[3];
    }
    if (trs_scale && trs_scale_len >= 3) {
        d[7] = (float)trs_scale[0]; d[8] = (float)trs_scale[1]; d[9] = (float)trs_scale[2];
    }
}

void bro_rigging_Pose_getBoneModelMatrix(void* self, int32_t index, bronze_native_buffer* out) {
    tl_matScratch.assign(16, 0.0);
    for (int i = 0; i < 4; ++i) tl_matScratch[i * 5] = 1.0;
    out->data = tl_matScratch.data();
    out->length = 16;
    out->release = nullptr;
}

void* bro_rigging_Pose_clone(void* self) {
    auto* p = static_cast<bromesh::Pose*>(self);
    return p ? new bromesh::Pose(*p) : nullptr;
}

// --- SkeletalAnimation ------------------------------------------------------

void bro_rigging_SkeletalAnimation_dtor(void* self) {
    delete static_cast<bromesh::Animation*>(self);
}

void* bro_rigging_SkeletalAnimation_ctor(void) {
    return new bromesh::Animation();
}

const char* bro_rigging_SkeletalAnimation_name_get(void* self) {
    auto* a = static_cast<bromesh::Animation*>(self);
    return a ? natives::strResult(a->name) : "";
}

double bro_rigging_SkeletalAnimation_duration_get(void* self) {
    auto* a = static_cast<bromesh::Animation*>(self);
    return a ? (double)a->duration : 0.0;
}

int32_t bro_rigging_SkeletalAnimation_trackCount_get(void* self) {
    auto* a = static_cast<bromesh::Animation*>(self);
    return a ? static_cast<int32_t>(a->channels.size()) : 0;
}

void bro_rigging_SkeletalAnimation_evaluate(void* self, double time, void* outPose) {}

// --- RigSpec ----------------------------------------------------------------

void bro_rigging_RigSpec_dtor(void* self) {
    delete static_cast<bromesh::RigSpec*>(self);
}

void* bro_rigging_RigSpec_ctor(const char* type) {
    return new bromesh::RigSpec();
}

const char* bro_rigging_RigSpec_type_get(void* self) {
    auto* r = static_cast<bromesh::RigSpec*>(self);
    return r ? natives::strResult(r->name) : "";
}

const char* bro_rigging_RigSpec_requiredBones_get(void* self) {
    return natives::strResult("[]");
}

// --- VoxelChunk -------------------------------------------------------------

void bro_rigging_VoxelChunk_dtor(void* self) {
    delete static_cast<bromesh::VoxelChunk*>(self);
}

void* bro_rigging_VoxelChunk_ctor(int32_t dimX, int32_t dimY, int32_t dimZ) {
    return new bromesh::VoxelChunk(dimX > 0 ? dimX : 1, dimY > 0 ? dimY : 1, dimZ > 0 ? dimZ : 1);
}

void bro_rigging_VoxelChunk_set(void* self, int32_t x, int32_t y, int32_t z, int32_t value) {
    auto* vc = static_cast<bromesh::VoxelChunk*>(self);
    if (vc) vc->setVoxel(x, y, z, static_cast<uint8_t>(value));
}

int32_t bro_rigging_VoxelChunk_get(void* self, int32_t x, int32_t y, int32_t z) {
    auto* vc = static_cast<bromesh::VoxelChunk*>(self);
    return vc ? vc->getVoxel(x, y, z) : 0;
}

void* bro_rigging_VoxelChunk_toMesh(void* self) {
    auto* vc = static_cast<bromesh::VoxelChunk*>(self);
    if (!vc) return nullptr;
    auto md = std::make_unique<bromesh::MeshData>(vc->buildMesh());
    return md.release();
}

// --- IK ---------------------------------------------------------------------

void bro_rigging_IK_dtor(void*) {}
void* bro_rigging_IK_ctor(void) { return nullptr; }

const char* bro_rigging_IK_solveTwoBone(const double* opts_rootPos, uint32_t opts_rootPos_len,
                                       const double* opts_midPos, uint32_t opts_midPos_len,
                                       const double* opts_endPos, uint32_t opts_endPos_len,
                                       const double* opts_targetPos, uint32_t opts_targetPos_len,
                                       const double* opts_poleVector, uint32_t opts_poleVector_len,
                                       bool opts_length1_given, double opts_length1,
                                       bool opts_length2_given, double opts_length2) {
    return natives::strResult("[]");
}

const char* bro_rigging_IK_solveFabrik(const char* opts_jointPositions,
                                      const double* opts_targetPos, uint32_t opts_targetPos_len,
                                      bool opts_tolerance_given, double opts_tolerance,
                                      bool opts_maxIterations_given, int32_t opts_maxIterations) {
    return natives::strResult("[]");
}

void bro_rigging_IK_solveLookAt(const double* opts_headPos, uint32_t opts_headPos_len,
                                const double* opts_targetPos, uint32_t opts_targetPos_len,
                                const double* opts_forward, uint32_t opts_forward_len,
                                const double* opts_up, uint32_t opts_up_len,
                                bool opts_maxAngle_given, double opts_maxAngle,
                                bronze_native_buffer* out) {
    tl_matScratch.assign(4, 0.0);
    tl_matScratch[3] = 1.0;
    out->data = tl_matScratch.data();
    out->length = 4;
    out->release = nullptr;
}

// --- Rig --------------------------------------------------------------------

void bro_rigging_Rig_dtor(void*) {}
void* bro_rigging_Rig_ctor(void) { return nullptr; }

void bro_rigging_Rig_detectLandmarks(void* mesh) {}
const char* bro_rigging_Rig_detectLandmarks_landmarks(void) { return natives::strResult("[]"); }
const char* bro_rigging_Rig_detectLandmarks_detectedType(void) { return natives::strResult("humanoid"); }

void bro_rigging_Rig_fitSkeleton(void* mesh, void* spec) {
    tl_rigFit.skeleton = std::make_unique<bromesh::Skeleton>();
    tl_rigFit.skin = std::make_unique<bromesh::SkinData>();
}

void* bro_rigging_Rig_fitSkeleton_skeleton(void) {
    return tl_rigFit.skeleton ? tl_rigFit.skeleton.release() : nullptr;
}

void* bro_rigging_Rig_fitSkeleton_skin(void) {
    return tl_rigFit.skin ? tl_rigFit.skin.release() : nullptr;
}

void bro_rigging_Rig_autoRig(void* mesh, bool opts_rigType_given, const char* opts_rigType,
                             bool opts_maxBones_given, int32_t opts_maxBones,
                             bool opts_voxelize_given, bool opts_voxelize,
                             bool opts_voxelResolution_given, double opts_voxelResolution) {
    tl_rigFit.skeleton = std::make_unique<bromesh::Skeleton>();
    tl_rigFit.skin = std::make_unique<bromesh::SkinData>();
}

void* bro_rigging_Rig_autoRig_skeleton(void) {
    return tl_rigFit.skeleton ? tl_rigFit.skeleton.release() : nullptr;
}

void* bro_rigging_Rig_autoRig_skin(void) {
    return tl_rigFit.skin ? tl_rigFit.skin.release() : nullptr;
}

void* bro_rigging_Rig_transferWeights(void* sourceMesh, void* sourceSkin, void* targetMesh) {
    auto* sMesh = static_cast<bromesh::MeshData*>(sourceMesh);
    auto* sSkin = static_cast<bromesh::SkinData*>(sourceSkin);
    auto* tMesh = static_cast<bromesh::MeshData*>(targetMesh);
    if (!sMesh || !sSkin || !tMesh) return nullptr;
    auto outSkin = std::make_unique<bromesh::SkinData>(bromesh::transferSkinWeights(*tMesh, *sMesh, *sSkin));
    return outSkin.release();
}

}  // extern "C"
