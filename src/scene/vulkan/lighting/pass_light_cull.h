#pragma once

#include "scene/vulkan/frame/scene_pass.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"

#include <vulkan/vulkan.h>

#include <array>

namespace bro::scene::vk {

/// Uploads per-frame bounded local lights (up to 1024) and interior enclosure
/// volumes (up to 64), and when local lights exist runs the 16x16 tiled compute
/// culling pass ahead of PassFrameUniforms.
class PassLightCull final : public ScenePass {
public:
    const char* name() const override { return "light-cull"; }
    bool setup(SceneGpu& gpu) override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    static constexpr int kSlots = 3;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::array<SceneVkBuffer, kSlots> tilesBuf_{};
    int slot_ = 0;
};

}  // namespace bro::scene::vk
