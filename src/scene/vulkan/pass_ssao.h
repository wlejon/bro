#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

struct SSAOUBOData {
    float proj[16];
    float invProj[16];
    float kernel[16 * 4];
    float params[4]; // x: radius, y: bias, z: noiseScaleX, w: noiseScaleY
};

class PassSSAO {
public:
    PassSSAO() = default;
    ~PassSSAO();

    PassSSAO(const PassSSAO&) = delete;
    PassSSAO& operator=(const PassSSAO&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    bool resize(SceneVkDevice& device, SceneVkAllocator& allocator, uint32_t width, uint32_t height);

    void render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                const SceneVkImage& depthImage,
                const float* projMatrix,
                const float* invProjMatrix,
                float radius, float bias);

    void applyAO(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                 VkImageView hdrTargetView,
                 uint32_t width, uint32_t height,
                 float intensity);

    const SceneVkImage& aoImage() const { return ssaoTex_[0]; }
    bool isValid() const { return ssaoTex_[0].isValid(); }

private:
    bool createPipelines(VkDevice device);
    bool createNoiseTexture(SceneVkDevice& device, SceneVkAllocator& allocator);
    void generateKernel();
    bool createTargets(SceneVkAllocator& allocator, uint32_t width, uint32_t height);
    void destroyTargets(SceneVkAllocator& allocator);

    uint32_t aoWidth_ = 0;
    uint32_t aoHeight_ = 0;

    VkDescriptorSetLayout ssaoDescLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout ssaoPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline ssaoPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout blurDescLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout applyAoDescLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout applyAoPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline applyAoPipeline_ = VK_NULL_HANDLE;

    SceneVkImage noiseTex_;
    SceneVkImage ssaoTex_[2]; // Half-res ping-pong textures
    VkSampler pointClampSampler_ = VK_NULL_HANDLE;
    VkSampler linearRepeatSampler_ = VK_NULL_HANDLE;

    float kernel_[16 * 4] = {};
};

} // namespace bro::scene::vk
