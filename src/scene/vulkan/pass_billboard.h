#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

struct alignas(16) BillboardPushConstants {
    float anchor[4];      // 16 bytes: xyz = worldAnchor, w = unused
    float right[4];       // 16 bytes: xyz = billboard right, w = unused
    float up[4];          // 16 bytes: xyz = billboard up, w = unused
    float halfSize[2];    // 8 bytes: halfW, halfH
    float uvMin[2];       // 8 bytes: uMin, vMin
    float uvMax[2];       // 8 bytes: uMax, vMax
    float strokeWidth;    // 4 bytes: in UV units
    int32_t shapeMode;    // 4 bytes: 0=rect, 1=circle SDF, 2=premul tex (Html), 3=ring disc, 4=straight tex (Sprite)
    float color[4];       // 16 bytes: RGBA color / tint
    float stroke[4];      // 16 bytes: RGBA stroke color
};

/// Camera-facing billboard and world-anchored quad rendering pass into HDR target.
class PassBillboard {
public:
    PassBillboard() = default;
    ~PassBillboard() = default;

    PassBillboard(const PassBillboard&) = delete;
    PassBillboard& operator=(const PassBillboard&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              VkDescriptorSetLayout cameraLayout,
              VkDescriptorSetLayout materialLayout,
              VkDescriptorSet defaultMaterialSet);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    void begin(VkCommandBuffer cmd, VkDescriptorSet cameraSet);
    void draw(VkCommandBuffer cmd, const BillboardPushConstants& push, VkDescriptorSet materialSet);

private:
    VkDescriptorSetLayout cameraLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkDescriptorSet defaultMaterialSet_ = VK_NULL_HANDLE;
    VkDescriptorSet activeCameraSet_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
