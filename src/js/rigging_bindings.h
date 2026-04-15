#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bromesh { struct SkinData; class VoxelChunk; }

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
};

} // namespace bro::js
