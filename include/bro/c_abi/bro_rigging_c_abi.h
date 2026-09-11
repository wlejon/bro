// =============================================================================
// bro_rigging_c_abi.h — Pure C-ABI declarations for bro.rigging
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_RIGGING_C_ABI_H
#define BRO_RIGGING_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.rigging.SkinData ---
void* bro_SkinData_create(void* opts);
void  bro_SkinData_destroy(void* self);
int32_t bro_SkinData_get_vertexCount(void* self);
int32_t bro_SkinData_get_maxWeights(void* self);
void* bro_SkinData_clone(void* self);
bool bro_SkinData_validate(void* self);
void bro_SkinData_normalize(void* self);

// --- Interface bro.rigging.Skeleton ---
void* bro_Skeleton_create(void* opts);
void  bro_Skeleton_destroy(void* self);
int32_t bro_Skeleton_get_boneCount(void* self);
int32_t bro_Skeleton_findBone(void* self, const char* name);
const char* bro_Skeleton_boneName(void* self, int32_t index);
int32_t bro_Skeleton_boneParent(void* self, int32_t index);
void* bro_Skeleton_boneBindPose(void* self, int32_t index);
void* bro_Skeleton_boneInverseBind(void* self, int32_t index);
void* bro_Skeleton_clone(void* self);

// --- Interface bro.rigging.Pose ---
void* bro_Pose_create(void* skeleton);
void  bro_Pose_destroy(void* self);
int32_t bro_Pose_get_boneCount(void* self);
void* bro_Pose_getBoneLocal(void* self, int32_t index);
void bro_Pose_setBoneLocal(void* self, int32_t index, void* trs);
void* bro_Pose_getBoneModelMatrix(void* self, int32_t index);
void* bro_Pose_clone(void* self);

// --- Interface bro.rigging.SkeletalAnimation ---
void* bro_SkeletalAnimation_create(void);
void  bro_SkeletalAnimation_destroy(void* self);
const char* bro_SkeletalAnimation_get_name(void* self);
double bro_SkeletalAnimation_get_duration(void* self);
int32_t bro_SkeletalAnimation_get_trackCount(void* self);
void bro_SkeletalAnimation_evaluate(void* self, double time, void* outPose);

// --- Interface bro.rigging.RigSpec ---
void* bro_RigSpec_create(const char* type);
void  bro_RigSpec_destroy(void* self);
const char* bro_RigSpec_get_type(void* self);
void* bro_RigSpec_get_requiredBones(void* self);

// --- Interface bro.rigging.VoxelChunk ---
void* bro_VoxelChunk_create(int32_t dimX, int32_t dimY, int32_t dimZ);
void  bro_VoxelChunk_destroy(void* self);
void bro_VoxelChunk_set(void* self, int32_t x, int32_t y, int32_t z, int32_t value);
int32_t bro_VoxelChunk_get(void* self, int32_t x, int32_t y, int32_t z);
void* bro_VoxelChunk_toMesh(void* self);

// --- Interface bro.rigging.IK ---
void* bro_IK_create(void);
void  bro_IK_destroy(void* self);
void* bro_IK_solveTwoBone(void* opts);
void* bro_IK_solveFabrik(void* opts);
void* bro_IK_solveLookAt(void* opts);

// --- Interface bro.rigging.Rig ---
void* bro_Rig_create(void);
void  bro_Rig_destroy(void* self);
void* bro_Rig_detectLandmarks(void* mesh);
void* bro_Rig_fitSkeleton(void* mesh, void* spec);
void* bro_Rig_autoRig(void* mesh, void* opts);
void* bro_Rig_transferWeights(void* sourceMesh, void* sourceSkin, void* targetMesh);

#ifdef __cplusplus
}
#endif

#endif // BRO_RIGGING_C_ABI_H
