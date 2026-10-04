#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>

namespace bro::scene::vk {

class PassColorLut {
public:
    PassColorLut() = default;
    ~PassColorLut();

    PassColorLut(const PassColorLut&) = delete;
    PassColorLut& operator=(const PassColorLut&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    bool updateLut(SceneVkDevice& device, SceneVkAllocator& allocator, int size, const uint8_t* rgbaVoxels);
    void clearLut(SceneVkAllocator& allocator);
    bool hasLut() const { return lutImage_.isValid(); }
    int lutSize() const { return lutSize_; }

    void render(VkCommandBuffer cmd, SceneVkDevice& device, SceneVkAllocator& allocator,
                SceneVkDescriptorPool& descPool,
                const SceneVkImage& inputImage,
                VkImageView outputTargetView,
                VkFormat outputFormat,
                uint32_t width, uint32_t height,
                float amount);

    VkDescriptorSetLayout descLayout() const { return descLayout_; }
    const SceneVkImage& lutImage() const { return lutImage_; }

private:
    bool createPipeline(VkDevice device);

    VkDescriptorSetLayout descLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    SceneVkImage lutImage_;
    int lutSize_ = 0;
};

} // namespace bro::scene::vk
