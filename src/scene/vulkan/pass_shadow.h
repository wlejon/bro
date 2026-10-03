#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// Uniform push constants for depth-only shadow passes (64 bytes).
struct alignas(16) ShadowPushConstants {
    float lightMVP[16];
};

/// Parameters for issuing a shadow caster static draw call.
struct ShadowCaster {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    uint32_t indexCount = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    float modelMatrix[16];
};

/// Parameters for issuing a shadow caster instanced draw call.
struct InstancedShadowCaster : public ShadowCaster {
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceSize instanceOffset = 0;
    uint32_t instanceCount = 0;
};

/// Parameters for issuing a shadow caster GPU-skinned draw call.
struct SkinnedShadowCaster : public ShadowCaster {
    VkBuffer skinAttribBuffer = VK_NULL_HANDLE;
    VkDeviceSize skinAttribOffset = 0;
    VkDescriptorSet bonePaletteSet = VK_NULL_HANDLE; // Set 0: 256 mat4 bone matrices UBO
};

/// Cascaded shadow map rendering pass into SceneVkShadowCascadeTarget.
class PassShadow {
public:
    struct Config {
        Config() = default;
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        float depthBiasConstant = 1.25f;
        float depthBiasSlope = 1.75f;
        float depthBiasClamp = 0.0f;
    };

    PassShadow() = default;
    ~PassShadow();

    PassShadow(const PassShadow&) = delete;
    PassShadow& operator=(const PassShadow&) = delete;

    /// Initialize shadow pipelines and layouts.
    bool init(SceneVkDevice& device);
    bool init(SceneVkDevice& device, const Config& config);

    /// Clean up shadow pipelines and layouts.
    void cleanup(SceneVkDevice& device);

    /// Begin rendering into a specific cascade index of the cascade shadow map target.
    void beginCascade(VkCommandBuffer cmd, SceneVkDevice& device,
                      SceneVkShadowCascadeTarget& target,
                      uint32_t cascadeIndex,
                      const float* lightViewProjMatrix);

    /// Draw a static shadow caster.
    void drawStatic(VkCommandBuffer cmd, const ShadowCaster& caster);

    /// Draw an instanced shadow caster batch.
    void drawInstanced(VkCommandBuffer cmd, const InstancedShadowCaster& caster);

    /// Draw a skinned shadow caster.
    void drawSkinned(VkCommandBuffer cmd, const SkinnedShadowCaster& caster);

    /// End rendering into the current cascade.
    void endCascade(VkCommandBuffer cmd, SceneVkDevice& device,
                    SceneVkShadowCascadeTarget& target);

    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout bonePaletteLayout() const { return bonePaletteLayout_; }

private:
    bool createPipelines(VkDevice device, const Config& config);

    Config config_{};
    VkDescriptorSetLayout bonePaletteLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    VkPipeline pipelineStatic_ = VK_NULL_HANDLE;
    VkPipeline pipelineInstanced_ = VK_NULL_HANDLE;
    VkPipeline pipelineSkinned_ = VK_NULL_HANDLE;

    // Active cascade state
    float activeLightVP_[16];
    uint32_t resolution_ = 0;
};

} // namespace bro::scene::vk
