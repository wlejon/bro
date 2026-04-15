#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bromesh { struct SkinData; struct Skeleton; struct Pose; class VoxelChunk; }

namespace bro::js {

/// Skeletal/animation/rigging JS bindings.
///
/// Step 1 (current): SkinData class, VoxelChunk class.
/// Later steps will add Skeleton, Pose, Animation, IK, Rig.
class RiggingBindings {
public:
    static void install(JSContext* ctx);
    static void cleanup(JSContext* ctx);

    /// Unwrap a JS SkinData -> bromesh::SkinData*, or nullptr.
    /// Used by mesh_bindings.cpp to implement Mesh.applySkinning.
    static bromesh::SkinData* getSkinData(JSContext* ctx, JSValueConst val);

    /// Wrap an existing SkinData into a fresh JS SkinData.
    /// Used to return SkinData from skin transfer / autoRig.
    static JSValue wrapSkinData(JSContext* ctx, bromesh::SkinData&& data);

    /// Skeleton accessors (cross-file use by Animation/IK/Rig bindings).
    static bromesh::Skeleton* getSkeleton(JSContext* ctx, JSValueConst val);
    static JSValue            wrapSkeleton(JSContext* ctx, bromesh::Skeleton&& skel);

    /// Pose accessors (cross-file use by Animation/IK).
    static bromesh::Pose* getPose(JSContext* ctx, JSValueConst val);
    static JSValue        wrapPose(JSContext* ctx, bromesh::Pose&& pose);
};

} // namespace bro::js
