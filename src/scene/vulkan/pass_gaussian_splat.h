#pragma once

// Gaussian splats: every visible splat node, its splats re-sorted back to
// front when the camera has moved enough, drawn as screen-space ellipses
// (premultiplied, depth-tested, no depth write) into the HDR scope. A node's
// sorted instance data lives in a device-local buffer rewritten only on a
// re-sort, keyed by node id and dropped when the node is destroyed.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <unordered_map>

namespace bro::scene {
class GaussianSplatNode;
}

namespace bro::scene::vk {

class PassGaussianSplat final : public ScenePass {
public:
    const char* name() const override { return "splats"; }
    bool setup(SceneGpu& gpu) override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void releaseNodes(SceneGpu& gpu, std::span<const uint32_t> ids) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// gaussian_splat.vert's UBO.
    struct Uniforms {
        float model[16];
        float view[16];
        float proj[16];
        float focal[2];
        float viewport[2];
    };
    struct Instances {
        SceneVkBuffer buffer;
        size_t capacity = 0;
    };

    void drawNode(SceneFrame& frame, VkPipeline pipeline, GaussianSplatNode& node);

    SceneVkDevice* device_ = nullptr;
    SceneVkAllocator* allocator_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
    SceneVkBuffer quad_;
    std::unordered_map<uint32_t, Instances> instances_;
};

}  // namespace bro::scene::vk
