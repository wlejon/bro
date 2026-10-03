#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target.h"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

/// Tonemapping operator mode.
enum class TonemapMode {
    Linear = 0,
    Reinhard = 1,
    ACES = 2
};

/// Parameters controlling the post-processing pipeline.
struct PostFxParams {
    float exposure = 1.0f;
    float gamma = 2.2f;
    float bloomThreshold = 1.0f;
    float bloomKnee = 0.5f;
    float bloomIntensity = 0.05f;
    TonemapMode tonemapMode = TonemapMode::ACES;
    bool enableBloom = true;
    bool enableFxaa = true;
};

/// Post-processing pipeline (bloom, tonemapping, and FXAA) rendering offscreen HDR
/// into a final LDR presentation target.
class PassPostFx {
public:
    struct Config {
        Config() = default;
        VkFormat presentationFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkFormat hdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    };

    PassPostFx() = default;
    ~PassPostFx();

    PassPostFx(const PassPostFx&) = delete;
    PassPostFx& operator=(const PassPostFx&) = delete;

    /// Initialize pipelines, descriptor layouts, and intermediate buffers.
    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              uint32_t width, uint32_t height);
    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              uint32_t width, uint32_t height,
              const Config& config);

    /// Resize intermediate targets.
    bool resize(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height);

    /// Free resources.
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    /// Execute the full post-processing chain:
    /// HDR color texture -> (Bloom) -> Tonemapping -> (FXAA) -> presentation target view.
    void render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                const SceneVkImage& hdrSceneImage,
                VkImageView presentationTargetView,
                VkFormat presentationFormat,
                uint32_t width, uint32_t height,
                const PostFxParams& params);

    VkPipelineLayout tonemapPipelineLayout() const { return tonemapLayout_; }
    VkPipelineLayout singleTexturePipelineLayout() const { return singleTexLayout_; }

private:
    bool createPipelines(VkDevice device, const Config& config);
    bool createIntermediateTargets(SceneVkAllocator& allocator, uint32_t width, uint32_t height);
    void destroyIntermediateTargets(SceneVkAllocator& allocator);

    Config config_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    VkDescriptorSetLayout singleTexDescLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapDescLayout_ = VK_NULL_HANDLE;

    VkPipelineLayout singleTexLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout_ = VK_NULL_HANDLE;

    VkPipeline pipelineBloomExtract_ = VK_NULL_HANDLE;
    VkPipeline pipelineBloomBlur_ = VK_NULL_HANDLE;
    VkPipeline pipelineTonemap_ = VK_NULL_HANDLE;
    VkPipeline pipelineFxaa_ = VK_NULL_HANDLE;

    VkSampler linearSampler_ = VK_NULL_HANDLE;

    // Intermediate render targets
    SceneVkImage bloomExtractImage_;
    SceneVkImage bloomBlurImage_;
    SceneVkImage intermediateLdrImage_;

    SceneVkDescriptorPool descPool_;
    VkDescriptorSet bloomExtractSet_ = VK_NULL_HANDLE;
    VkDescriptorSet bloomBlurSet_ = VK_NULL_HANDLE;
    VkDescriptorSet tonemapSet_ = VK_NULL_HANDLE;
    VkDescriptorSet fxaaSet_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
