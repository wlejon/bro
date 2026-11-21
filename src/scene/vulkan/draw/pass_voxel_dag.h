#pragma once

// SVDAG world rendering passes:
//   PassVoxelDagPreDepth  snapshots the opaque raster depth before voxel rays
//                         draw so raster foreground geometry occludes voxels.
//   PassVoxelDag          raymarches visible VoxelWorldNode regions into HDR,
//                         optional SSAO indirect buffer, and reversed-Z depth.

#include "scene/vulkan/frame/scene_pass.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

namespace bro::scene {
class VoxelWorldNode;
}

namespace bro::scene::vk {

class PassVoxelDagPreDepth final : public ScenePass {
public:
    const char* name() const override { return "voxeldag_pre_depth"; }
    bool setup(SceneGpu& /*gpu*/) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& /*gpu*/) override {}
};

class PassVoxelDag final : public ScenePass {
public:
    const char* name() const override { return "voxeldag"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) override;
    void cleanup(SceneGpu& gpu) override;

private:
    struct alignas(16) ParamsUbo {
        uint32_t counts[4] = {};  // regionCount, animTick, maxSteps, shadowSteps
        float scales[4] = {};     // lodScale, pixelCone, 0, 0
    };

    struct GpuPool {
        SceneVkBuffer nodes;
        SceneVkBuffer children;
        SceneVkBuffer bricks;
        SceneVkBuffer attrs;
        SceneVkBuffer animClips;
        SceneVkBuffer animFrames;
        uint64_t uploadedGen = 0;
    };

    GpuPool& syncNodePool(SceneGpu& gpu, uint32_t nodeId, const VoxelWorldNode& node);

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout set2Layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkShaderModule fsIndirect_ = VK_NULL_HANDLE;
    SceneVkBuffer dummyBuf_;
    PipelineVariants pipelines_;
    std::unordered_map<uint32_t, GpuPool> nodePools_;
};

}  // namespace bro::scene::vk
