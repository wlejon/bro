#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <unordered_map>

namespace bro::scene {
class GaussianSplatNode;
}

namespace bro::scene::vk {

struct SplatUBOData {
    float model[16];
    float view[16];
    float proj[16];
    float focal[2];
    float viewport[2];
};

class PassGaussianSplat {
public:
    PassGaussianSplat() = default;
    ~PassGaussianSplat();

    PassGaussianSplat(const PassGaussianSplat&) = delete;
    PassGaussianSplat& operator=(const PassGaussianSplat&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT,
              VkFormat depthFormat = VK_FORMAT_D32_SFLOAT,
              VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);
    /// Rebuild the pipeline for the HDR target's sample count.
    bool setSampleCount(VkDevice dev, VkSampleCountFlagBits samples);

    void renderNode(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                    GaussianSplatNode* node,
                    const float* viewMatrix,
                    const float* projMatrix,
                    const float eye[3],
                    uint32_t width, uint32_t height);

private:
    bool createPipelines(VkDevice device, VkFormat colorFormat, VkFormat depthFormat, VkSampleCountFlagBits samples);
    void destroyPipelines(VkDevice device);

    VkFormat colorFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    bool createQuadBuffer(SceneVkAllocator& allocator);

    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    SceneVkBuffer quadBuffer_;

    struct NodeGpuData {
        SceneVkBuffer instanceBuffer;
        size_t capacityBytes = 0;
    };
    std::unordered_map<const void*, NodeGpuData> nodeCache_;
};

} // namespace bro::scene::vk
