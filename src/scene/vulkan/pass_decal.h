#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

struct alignas(16) DecalPushConstants {
    float model[16];        // 64 bytes
    float invModel[16];     // 64 bytes
    float modulate[4];      // 16 bytes: rgb tint, a = master opacity
    float decalUp[4];       // 16 bytes: xyz = up (unit), w = emissionStrength
    float fades[4];         // 16 bytes: x = upperFade, y = lowerFade, z = normalFade, w = unused
    int32_t flags[4];       // 16 bytes: x = hasAlbedo, y = hasEmission
};

/// Screen-space projected decal rendering pass.
class PassDecal {
public:
    PassDecal() = default;
    ~PassDecal() = default;

    PassDecal(const PassDecal&) = delete;
    PassDecal& operator=(const PassDecal&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout lightingLayout);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    void begin(VkCommandBuffer cmd, VkDescriptorSet cameraSet, VkDescriptorSet lightingSet);

    void draw(VkCommandBuffer cmd, const DecalPushConstants& push, VkDescriptorSet materialSet);

    VkDescriptorSetLayout materialLayout() const { return materialLayout_; }
    VkDescriptorSet createMaterialSet(VkImageView depthView, VkSampler depthSampler,
                                      VkImageView albedoView, VkSampler albedoSampler,
                                      VkImageView emissionView, VkSampler emissionSampler,
                                      SceneVkDescriptorPool& pool);

    VkImageView dummyWhiteView() const { return dummyWhiteImage_.view; }
    VkImageView dummyBlackView() const { return dummyBlackImage_.view; }
    VkSampler defaultSampler() const { return defaultSampler_; }

private:
    VkDescriptorSetLayout cameraLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    SceneVkBuffer cubeVertexBuffer_;

    SceneVkImage dummyWhiteImage_;
    SceneVkImage dummyBlackImage_;
    VkSampler defaultSampler_ = VK_NULL_HANDLE;

    VkDescriptorSet activeCameraSet_ = VK_NULL_HANDLE;
    VkDescriptorSet activeLightingSet_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
