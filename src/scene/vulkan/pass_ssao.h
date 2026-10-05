#pragma once

// Screen-space ambient occlusion, applied to indirect light only.
//
// While SSAO is on, the opaque scope carries a second colour attachment that
// mesh.frag (and the terrain) fill with each surface's indirect light —
// ambient plus probe reflection, after shade-map and fog. Then:
//
//   PassSSAO     estimates visibility at half resolution from the depth
//                snapshot (hemisphere kernel, rotation noise) and blurs it
//                separably, into its own R8 targets.
//   PassAoApply  subtracts indirect * (1 - visibility) from the HDR colour,
//                full-screen in the HDR scope, before decals, SSR and the
//                translucents draw: direct light and emission keep their
//                full value, and nothing drawn later is darkened.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassSSAO final : public ScenePass {
public:
    const char* name() const override { return "ssao"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

    /// The blurred visibility of this frame (valid after record()).
    SceneVkImage& visibility() { return ao_[0]; }

private:
    struct Uniforms {
        float proj[16];
        float invProj[16];
        float kernel[16 * 4];
        float params[4];   // radius, bias, noise scale x, y
    };

    void generateKernel();
    bool createNoise(SceneGpu& gpu);
    void draw(SceneFrame& frame, SceneVkImage& target, VkPipeline pipeline, VkPipelineLayout layout,
              VkDescriptorSet set, const float* push);

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout ssaoSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout ssaoLayout_ = VK_NULL_HANDLE;
    VkPipeline ssaoPipeline_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout blurSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;

    SceneVkImage noise_;
    SceneVkImage ao_[2];   // half-res ping-pong
    VkSampler clampSampler_ = VK_NULL_HANDLE;
    float kernel_[16 * 4] = {};
    uint8_t noisePixels_[16 * 4] = {};   // the 4x4 rotation noise, RGBA8
};

class PassAoApply final : public ScenePass {
public:
    explicit PassAoApply(PassSSAO& ssao) : ssao_(ssao) {}

    const char* name() const override { return "ao-apply"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    PassSSAO& ssao_;
    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
