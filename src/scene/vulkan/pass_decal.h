#pragma once

// Projected decals: each DecalNode's unit box is rasterised (back faces, so
// a camera inside the box still sees it) and every covered pixel whose
// opaque surface lies inside the box takes the decal's albedo and emission,
// reconstructed from the depth snapshot. Drawn in the HDR scope after the
// opaque surfaces and SSR, before translucents, in renderPriority order.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class PassDecal final : public ScenePass {
public:
    const char* name() const override { return "decals"; }
    bool setup(SceneGpu& gpu) override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// decal.vert/frag's push block.
    struct alignas(16) Push {
        float model[16];
        float invModel[16];
        float modulate[4];   // rgb tint, a = opacity
        float decalUp[4];    // xyz unit up, w = emission strength
        float fades[4];      // upper, lower, normal
        int32_t flags[4];    // has albedo, has emission
    };

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;   // depth, albedo, emission
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    SceneVkBuffer cube_;   // 36 vertices of the unit box
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
