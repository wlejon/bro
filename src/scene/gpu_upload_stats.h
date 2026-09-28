#pragma once

// Process-wide counters of the scene's GPU uploads: every mesh geometry
// upload (vertex + index buffers, MeshNode and InstancedMeshNode) and every
// material texture upload. What a test asserts to prove a scene reached the
// GPU once — e.g. that moving a SceneGraph to another canvas
// (SceneGraph.attachTo) re-uploads nothing. Read through the headless
// `__host.sceneUploadStats()`. Scene rendering runs on the main thread, but
// the counters are relaxed atomics so a read from anywhere is well-defined.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace bro::scene {

struct GpuUploadStats {
    std::atomic<uint64_t> meshUploads{0};
    std::atomic<uint64_t> meshBytes{0};
    std::atomic<uint64_t> textureUploads{0};
    std::atomic<uint64_t> textureBytes{0};
};

inline GpuUploadStats& gpuUploadStats() {
    static GpuUploadStats stats;
    return stats;
}

inline void noteMeshUpload(size_t bytes) {
    gpuUploadStats().meshUploads.fetch_add(1, std::memory_order_relaxed);
    gpuUploadStats().meshBytes.fetch_add(bytes, std::memory_order_relaxed);
}

inline void noteTextureUpload(size_t bytes) {
    gpuUploadStats().textureUploads.fetch_add(1, std::memory_order_relaxed);
    gpuUploadStats().textureBytes.fetch_add(bytes, std::memory_order_relaxed);
}

}  // namespace bro::scene
