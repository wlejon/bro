#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target.h"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

/// Uniform push constants for environment and atmosphere rendering (128 bytes).
struct alignas(16) EnvironmentPushConstants {
    float invViewProj[16];   // 64 bytes: screen UV -> world ray reconstruction
    float sunDir[4];         // 16 bytes: xyz = sun dir, w = hasCubemap flag (0 or 1)
    float skyColor[4];       // 16 bytes: rgb = sky color, a = intensity
    float horizonColor[4];   // 16 bytes: rgb = horizon color, a = sun size
    float groundColor[4];    // 16 bytes: rgb = ground color, a = sun intensity
};

/// Parameters for rendering the environment/sky pass.
struct EnvironmentParams {
    float invViewProj[16];
    float sunDirection[3] = {0.0f, -1.0f, 0.0f};
    float skyColor[3] = {0.2f, 0.4f, 0.8f};
    float horizonColor[3] = {0.7f, 0.75f, 0.8f};
    float groundColor[3] = {0.2f, 0.18f, 0.15f};
    float sunIntensity = 2.0f;
    float skyIntensity = 1.0f;
    bool hasCubemap = false;
};

/// Skybox, procedural atmosphere, and IBL environment pass.
class PassEnvironment {
public:
    struct Config {
        Config() = default;
        VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        bool depthTest = true;
        VkCompareOp depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // Reversed-Z
    };

    PassEnvironment() = default;
    ~PassEnvironment();

    PassEnvironment(const PassEnvironment&) = delete;
    PassEnvironment& operator=(const PassEnvironment&) = delete;

    /// Initialize shaders, pipeline layout, dummy cubemap, and graphics pipeline.
    bool init(SceneVkDevice& device, SceneVkAllocator& allocator);
    bool init(SceneVkDevice& device, SceneVkAllocator& allocator, const Config& config);

    /// Clean up environment pipeline and resources.
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    /// Record environment pass draw into active dynamic rendering pass.
    void render(VkCommandBuffer cmd, uint32_t width, uint32_t height,
                const EnvironmentParams& params,
                VkDescriptorSet cubemapSet = VK_NULL_HANDLE);

    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout cubemapLayout() const { return cubemapLayout_; }
    VkDescriptorSet defaultCubemapSet() const { return defaultCubemapSet_; }

private:
    bool createDefaultCubemap(SceneVkDevice& device, SceneVkAllocator& allocator);
    bool createPipeline(VkDevice device, const Config& config);

    VkDescriptorSetLayout cubemapLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    // Dummy cubemap fallback
    SceneVkImage dummyCubemapImage_;
    VkSampler cubemapSampler_ = VK_NULL_HANDLE;
    SceneVkDescriptorPool descPool_;
    VkDescriptorSet defaultCubemapSet_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
