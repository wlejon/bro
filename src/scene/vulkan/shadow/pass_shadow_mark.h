#pragma once

// The shadow pages' marking (docs/scene-world-architecture.md §8): after the
// opaque surfaces, a compute pass over the depth target marks, for every
// shadowed light, the page each pixel's lookup needs (shaders/shadow/
// shadow_mark.comp): the sun's clipmap level its footprint wants (a texel a
// pixel), the next one in the hand-over band, and the guard SP_GUARD_LEVELS
// coarser; a spot's or a point light's face's mip, and its one-page top. A
// pixel near a page's edge marks the neighbour its filter and normal offset
// may reach. One invocation marks for a block of kStep x kStep pixels (its
// centre's depth), a bit a page-table entry, set once (a word read first).
//
// The bits are copied out after the pass (SceneShadowPages::
// recordReadback) and read once the frame's ticket completes: they name the
// plan this frame drew with, and the paging takes them before the next plan.

#include "scene/vulkan/frame/scene_pass.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassShadowMark final : public ScenePass {
public:
    /// Pixels a side an invocation marks for.
    static constexpr uint32_t kStep = 2;

    const char* name() const override { return "shadow:mark"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

}  // namespace bro::scene::vk
