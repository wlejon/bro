#pragma once

// Internal declarations and shared helpers for the bronze host rigging subsystem.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "embed/embed.h"

#include <bromesh/mesh_data.h>
#include <bromesh/manipulation/skin.h>
#include <bromesh/manipulation/skin_transfer.h>
#include <bromesh/animation/pose.h>
#include <bromesh/animation/ik.h>
#include <bromesh/animation/locomotion.h>
#include <bromesh/animation/retarget.h>
#include <bromesh/rigging/rig_spec.h>
#include <bromesh/rigging/landmarks.h>
#include <bromesh/rigging/landmark_detect.h>
#include <bromesh/rigging/skeleton_fit.h>
#include <bromesh/rigging/auto_rig.h>
#include <bromesh/rigging/skin_validate.h>
#include <bromesh/voxel/voxel_chunk.h>
#include <bromesh/io/gltf.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::bronze_host {

namespace ev = bronze::embed;

inline bromesh::SkinData& S(void* h) { return *static_cast<bromesh::SkinData*>(h); }
inline bromesh::Skeleton& K(void* h) { return *static_cast<bromesh::Skeleton*>(h); }
inline bromesh::Pose& P(void* h) { return *static_cast<bromesh::Pose*>(h); }
inline bromesh::Animation& A(void* h) { return *static_cast<bromesh::Animation*>(h); }
inline bromesh::RigSpec& R(void* h) { return *static_cast<bromesh::RigSpec*>(h); }
inline bromesh::VoxelChunk& V(void* h) { return *static_cast<bromesh::VoxelChunk*>(h); }
inline bromesh::MeshData& M(void* h) { return *static_cast<bromesh::MeshData*>(h); }

template <typename T>
void copyOut(std::span<const T> src, bronze_native_buffer* out) {
    out->data = const_cast<T*>(src.data());
    out->length = static_cast<uint32_t>(src.size());
    out->release = nullptr;
}

template <typename T>
void copyOut(const std::vector<T>& src, bronze_native_buffer* out) {
    copyOut(std::span<const T>(src), out);
}

// Class names
inline constexpr const char* kSkinData = "__bro_native.rigging.SkinData";
inline constexpr const char* kSkeleton = "__bro_native.rigging.Skeleton";
inline constexpr const char* kPose = "__bro_native.rigging.Pose";
inline constexpr const char* kAnimation = "__bro_native.rigging.Animation";
inline constexpr const char* kRigSpec = "__bro_native.rigging.RigSpec";
inline constexpr const char* kVoxelChunk = "__bro_native.rigging.VoxelChunk";
inline constexpr const char* kMesh = "__bro_native.mesh.Mesh";

inline ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params,
                               ev::NativeKind kind = ev::NativeKind::Function) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

inline bool fn(const char* path, void* f, const char* ret, std::initializer_list<const char*> params,
               std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

inline bool ctor(const char* path, void* f, ev::HandleDestructor dtor, bronze::runtime::Finalize finalize,
                 std::initializer_list<const char*> params, std::string* error) {
    ev::NativeSignature s = sig(path, params, ev::NativeKind::Constructor);
    s.className = path;
    s.destructor = dtor;
    s.finalize = finalize;
    return ev::registerNative(path, f, s, error);
}

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

bool registerRiggingCoreNatives(std::string* error);
bool registerRiggingAnimNatives(std::string* error);

}  // namespace bro::bronze_host
