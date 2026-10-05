#pragma once

// Depth of field on the HDR image, before bloom and tonemapping: the HDR
// colour is downsampled to half resolution and Gaussian-blurred separably
// (radius = max blur), then composited with the sharp image by each pixel's
// distance from the focus plane (from the depth snapshot) into dofHdr, which
// the post chain reads instead of hdr.

#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassDoF final : public ScenePass {
public:
    const char* name() const override { return "depth-of-field"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;   // linear, clamp
    VkDescriptorSetLayout blurSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;
    SceneVkImage blur_[2];   // half-res ping-pong
};

}  // namespace bro::scene::vk
