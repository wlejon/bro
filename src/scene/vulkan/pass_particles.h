#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

struct alignas(16) ParticlePushConstants {
    float model[16];   // 64 bytes
    float camRight[4]; // 16 bytes: xyz = right, w = unused
    float camUp[4];    // 16 bytes: xyz = up, w = unused
    float params[4];   // 16 bytes: x=cols, y=rows, z=mode (0=point, 1=tex), w=softDist
};

/// 3D particle systems rendering pass into HDR target.
class PassParticles {
public:
    PassParticles() = default;
    ~PassParticles() = default;

    PassParticles(const PassParticles&) = delete;
    PassParticles& operator=(const PassParticles&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              VkDescriptorSetLayout cameraLayout);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);
    /// Rebuild the pipelines for the HDR target's sample count.
    bool setSampleCount(VkDevice dev, VkSampleCountFlagBits samples);

    void begin(VkCommandBuffer cmd, VkDescriptorSet cameraSet);

    void draw(VkCommandBuffer cmd, bool additive,
              VkBuffer instanceBuffer, VkDeviceSize instanceOffset, uint32_t instanceCount,
              const ParticlePushConstants& push,
              VkDescriptorSet materialSet);

    VkDescriptorSetLayout materialLayout() const { return materialLayout_; }
    VkDescriptorSet defaultMaterialSet() const { return defaultMaterialSet_; }

    VkDescriptorSet createParticleMaterialSet(SceneVkDevice& device, VkImageView imageView, VkSampler sampler,
                                              VkImageView depthView, VkSampler depthSampler);

private:
    bool createPipelines(VkDevice dev);

    VkSampleCountFlagBits samples_ = VK_SAMPLE_COUNT_1_BIT;
    VkDescriptorSetLayout cameraLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipelineNormal_ = VK_NULL_HANDLE;
    VkPipeline pipelineAdditive_ = VK_NULL_HANDLE;

    SceneVkImage dummyWhiteImage_;
    VkSampler defaultSampler_ = VK_NULL_HANDLE;
    SceneVkDescriptorPool defaultDescPool_;
    VkDescriptorSet defaultMaterialSet_ = VK_NULL_HANDLE;

    VkDescriptorSet activeCameraSet_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
