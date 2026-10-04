#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

struct SSRParams {
    bool isPerspective = true;
    float maxDistance = 30.0f;
    int steps = 48;
    float thickness = 0.3f;
    float intensity = 1.0f;
    float edgeFade = 0.1f;
};

struct SSRUBOData {
    float proj[16];
    float invProj[16];
    float params1[4]; // x: isPerspective, y: maxDistance, z: steps, w: thickness
    float params2[4]; // x: intensity, y: edgeFade, zw: pad
};

class PassSSR {
public:
    PassSSR() = default;
    ~PassSSR();

    PassSSR(const PassSSR&) = delete;
    PassSSR& operator=(const PassSSR&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    void render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                SceneVkDescriptorPool& descPool,
                const SceneVkImage& colorSnapshot,
                const SceneVkImage& depthImage,
                VkImageView outputTargetView,
                uint32_t width, uint32_t height,
                const float* projMatrix,
                const float* invProjMatrix,
                const SSRParams& params);

private:
    bool createPipeline(VkDevice device);

    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    SceneVkBuffer ssrUbo_;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

} // namespace bro::scene::vk
