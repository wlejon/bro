#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

struct DoFParams {
    float focusDistance = 10.0f;
    float focusRange = 5.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    bool isPerspective = true;
    float maxBlur = 4.0f;
};

class PassDoF {
public:
    PassDoF() = default;
    ~PassDoF();

    PassDoF(const PassDoF&) = delete;
    PassDoF& operator=(const PassDoF&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    bool resize(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height);

    void render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                const SceneVkImage& sharpHdrImage,
                const SceneVkImage& depthImage,
                VkImageView outputTargetView,
                uint32_t width, uint32_t height,
                const DoFParams& params);

private:
    bool createPipelines(VkDevice device);
    bool createIntermediateTargets(SceneVkAllocator& allocator, uint32_t width, uint32_t height);
    void destroyIntermediateTargets(SceneVkAllocator& allocator);

    uint32_t halfW_ = 0;
    uint32_t halfH_ = 0;

    VkDescriptorSetLayout singleTexDescLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout singleTexPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline downsamplePipeline_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout dofDescLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout dofPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline dofPipeline_ = VK_NULL_HANDLE;

    SceneVkImage blurTex_[2]; // Half-res ping-pong targets
    VkSampler linearClampSampler_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
