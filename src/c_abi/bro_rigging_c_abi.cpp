// =============================================================================
// bro_rigging_c_abi.cpp — C++ forwarding implementations for bro.rigging
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_rigging_c_abi.h"
#include <cstdint>
#include <string>
#include <vector>

struct BroSkinDataImpl {
    int32_t vertexCount = 0;
    int32_t maxWeights = 4;
};

struct BroSkeletonImpl {
    int32_t boneCount = 0;
    std::vector<std::string> boneNames;
    std::vector<int32_t> boneParents;
};

struct BroPoseImpl {
    int32_t boneCount = 0;
};

struct BroSkeletalAnimationImpl {
    std::string name;
    double duration = 0.0;
    int32_t trackCount = 0;
};

struct BroRigSpecImpl {
    std::string type;
    std::vector<std::string> requiredBones;
};

struct BroVoxelChunkImpl {
    int32_t dimX = 1;
    int32_t dimY = 1;
    int32_t dimZ = 1;
    std::vector<int32_t> voxels;

    BroVoxelChunkImpl(int32_t x, int32_t y, int32_t z)
        : dimX(x > 0 ? x : 1), dimY(y > 0 ? y : 1), dimZ(z > 0 ? z : 1) {
        voxels.resize(static_cast<size_t>(dimX) * dimY * dimZ, 0);
    }

    size_t index(int32_t x, int32_t y, int32_t z) const {
        if (x < 0 || x >= dimX || y < 0 || y >= dimY || z < 0 || z >= dimZ) return SIZE_MAX;
        return static_cast<size_t>(x) + static_cast<size_t>(dimX) * (static_cast<size_t>(y) + static_cast<size_t>(dimY) * static_cast<size_t>(z));
    }
};

struct BroIKImpl {
};

struct BroRigImpl {
};

extern "C" {

// --- Interface bro.rigging.SkinData ---
void* bro_SkinData_create(void* /*opts*/) {
    return new BroSkinDataImpl();
}

void bro_SkinData_destroy(void* self) {
    delete static_cast<BroSkinDataImpl*>(self);
}

int32_t bro_SkinData_get_vertexCount(void* self) {
    auto* s = static_cast<BroSkinDataImpl*>(self);
    return s ? s->vertexCount : 0;
}

int32_t bro_SkinData_get_maxWeights(void* self) {
    auto* s = static_cast<BroSkinDataImpl*>(self);
    return s ? s->maxWeights : 4;
}

void* bro_SkinData_clone(void* self) {
    auto* s = static_cast<BroSkinDataImpl*>(self);
    auto* c = new BroSkinDataImpl();
    if (s) *c = *s;
    return c;
}

bool bro_SkinData_validate(void* /*self*/) {
    return true;
}

void bro_SkinData_normalize(void* /*self*/) {
}

// --- Interface bro.rigging.Skeleton ---
void* bro_Skeleton_create(void* /*opts*/) {
    return new BroSkeletonImpl();
}

void bro_Skeleton_destroy(void* self) {
    delete static_cast<BroSkeletonImpl*>(self);
}

int32_t bro_Skeleton_get_boneCount(void* self) {
    auto* s = static_cast<BroSkeletonImpl*>(self);
    return s ? s->boneCount : 0;
}

int32_t bro_Skeleton_findBone(void* self, const char* name) {
    auto* s = static_cast<BroSkeletonImpl*>(self);
    if (!s || !name) return -1;
    for (size_t i = 0; i < s->boneNames.size(); ++i) {
        if (s->boneNames[i] == name) return static_cast<int32_t>(i);
    }
    return -1;
}

const char* bro_Skeleton_boneName(void* self, int32_t index) {
    auto* s = static_cast<BroSkeletonImpl*>(self);
    if (!s || index < 0 || static_cast<size_t>(index) >= s->boneNames.size()) return "";
    return s->boneNames[static_cast<size_t>(index)].c_str();
}

int32_t bro_Skeleton_boneParent(void* self, int32_t index) {
    auto* s = static_cast<BroSkeletonImpl*>(self);
    if (!s || index < 0 || static_cast<size_t>(index) >= s->boneParents.size()) return -1;
    return s->boneParents[static_cast<size_t>(index)];
}

void* bro_Skeleton_boneBindPose(void* /*self*/, int32_t /*index*/) {
    return nullptr;
}

void* bro_Skeleton_boneInverseBind(void* /*self*/, int32_t /*index*/) {
    return nullptr;
}

void* bro_Skeleton_clone(void* self) {
    auto* s = static_cast<BroSkeletonImpl*>(self);
    auto* c = new BroSkeletonImpl();
    if (s) *c = *s;
    return c;
}

// --- Interface bro.rigging.Pose ---
void* bro_Pose_create(void* skeleton) {
    auto* p = new BroPoseImpl();
    if (skeleton) {
        auto* s = static_cast<BroSkeletonImpl*>(skeleton);
        p->boneCount = s->boneCount;
    }
    return p;
}

void bro_Pose_destroy(void* self) {
    delete static_cast<BroPoseImpl*>(self);
}

int32_t bro_Pose_get_boneCount(void* self) {
    auto* p = static_cast<BroPoseImpl*>(self);
    return p ? p->boneCount : 0;
}

void* bro_Pose_getBoneLocal(void* /*self*/, int32_t /*index*/) {
    return nullptr;
}

void bro_Pose_setBoneLocal(void* /*self*/, int32_t /*index*/, void* /*trs*/) {
}

void* bro_Pose_getBoneModelMatrix(void* /*self*/, int32_t /*index*/) {
    return nullptr;
}

void* bro_Pose_clone(void* self) {
    auto* p = static_cast<BroPoseImpl*>(self);
    auto* c = new BroPoseImpl();
    if (p) *c = *p;
    return c;
}

// --- Interface bro.rigging.SkeletalAnimation ---
void* bro_SkeletalAnimation_create(void) {
    return new BroSkeletalAnimationImpl();
}

void bro_SkeletalAnimation_destroy(void* self) {
    delete static_cast<BroSkeletalAnimationImpl*>(self);
}

const char* bro_SkeletalAnimation_get_name(void* self) {
    auto* a = static_cast<BroSkeletalAnimationImpl*>(self);
    return a ? a->name.c_str() : "";
}

double bro_SkeletalAnimation_get_duration(void* self) {
    auto* a = static_cast<BroSkeletalAnimationImpl*>(self);
    return a ? a->duration : 0.0;
}

int32_t bro_SkeletalAnimation_get_trackCount(void* self) {
    auto* a = static_cast<BroSkeletalAnimationImpl*>(self);
    return a ? a->trackCount : 0;
}

void bro_SkeletalAnimation_evaluate(void* /*self*/, double /*time*/, void* /*outPose*/) {
}

// --- Interface bro.rigging.RigSpec ---
void* bro_RigSpec_create(const char* type) {
    auto* r = new BroRigSpecImpl();
    if (type) r->type = type;
    return r;
}

void bro_RigSpec_destroy(void* self) {
    delete static_cast<BroRigSpecImpl*>(self);
}

const char* bro_RigSpec_get_type(void* self) {
    auto* r = static_cast<BroRigSpecImpl*>(self);
    return r ? r->type.c_str() : "";
}

void* bro_RigSpec_get_requiredBones(void* /*self*/) {
    return nullptr;
}

// --- Interface bro.rigging.VoxelChunk ---
void* bro_VoxelChunk_create(int32_t dimX, int32_t dimY, int32_t dimZ) {
    return new BroVoxelChunkImpl(dimX, dimY, dimZ);
}

void bro_VoxelChunk_destroy(void* self) {
    delete static_cast<BroVoxelChunkImpl*>(self);
}

void bro_VoxelChunk_set(void* self, int32_t x, int32_t y, int32_t z, int32_t value) {
    auto* v = static_cast<BroVoxelChunkImpl*>(self);
    if (!v) return;
    size_t idx = v->index(x, y, z);
    if (idx != SIZE_MAX) v->voxels[idx] = value;
}

int32_t bro_VoxelChunk_get(void* self, int32_t x, int32_t y, int32_t z) {
    auto* v = static_cast<BroVoxelChunkImpl*>(self);
    if (!v) return 0;
    size_t idx = v->index(x, y, z);
    return idx != SIZE_MAX ? v->voxels[idx] : 0;
}

void* bro_VoxelChunk_toMesh(void* /*self*/) {
    return nullptr;
}

// --- Interface bro.rigging.IK ---
void* bro_IK_create(void) {
    return new BroIKImpl();
}

void bro_IK_destroy(void* self) {
    delete static_cast<BroIKImpl*>(self);
}

void* bro_IK_solveTwoBone(void* /*opts*/) {
    return nullptr;
}

void* bro_IK_solveFabrik(void* /*opts*/) {
    return nullptr;
}

void* bro_IK_solveLookAt(void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.rigging.Rig ---
void* bro_Rig_create(void) {
    return new BroRigImpl();
}

void bro_Rig_destroy(void* self) {
    delete static_cast<BroRigImpl*>(self);
}

void* bro_Rig_detectLandmarks(void* /*mesh*/) {
    return nullptr;
}

void* bro_Rig_fitSkeleton(void* /*mesh*/, void* /*spec*/) {
    return nullptr;
}

void* bro_Rig_autoRig(void* /*mesh*/, void* /*opts*/) {
    return nullptr;
}

void* bro_Rig_transferWeights(void* /*sourceMesh*/, void* /*sourceSkin*/, void* /*targetMesh*/) {
    return nullptr;
}

}
