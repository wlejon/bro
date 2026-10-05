#pragma once

// Screen-space reflections on the opaque surfaces, drawn full-screen into the
// HDR scope (at the scene's sample count) before decals and translucents.
// Ray-marches the depth snapshot along the reflected view ray and mixes the
// hit's colour from the colour snapshot over the surface by its reflectance
// (the alpha mesh.frag writes when SSR is on); sky pixels are left alone.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassSSR final : public ScenePass {
public:
    const char* name() const override { return "ssr"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// ssr.frag's uniform block.
    struct Uniforms {
        float proj[16];
        float invProj[16];
        float params1[4];   // perspective, maxDistance, steps, thickness
        float params2[4];   // intensity, edgeFade
    };

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
