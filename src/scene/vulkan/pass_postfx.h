#pragma once

// HDR to LDR: optional bloom (bright-pass extract and blur at half
// resolution), tonemapping (linear / Reinhard / ACES, exposure, gamma) and
// optional FXAA. Reads dofHdr when depth of field ran, else hdr; writes ldr,
// or postLdr when colour grading follows.

#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassPostFx final : public ScenePass {
public:
    const char* name() const override { return "postfx"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// bloom.frag's push block (blur.frag-compatible single-texture layout).
    struct alignas(16) BloomPush {
        float threshold;
        float knee;
        float blurRadius;
        int32_t passType;   // 0 bright-pass extract, 1 blur
    };
    /// tonemap.frag's push block.
    struct alignas(16) TonemapPush {
        float exposure;
        float gamma;
        float bloomIntensity;
        int32_t tonemapMode;   // SceneRenderer::ToneMap
        int32_t enableFxaa;
        float texelSize[2];
        float padding;
    };

    static SceneVkImage& source(const SceneFrame& frame);
    static SceneVkImage& output(const SceneFrame& frame);
    void bloom(SceneFrame& frame, const SceneVkImage& hdr);
    void pass(SceneFrame& frame, SceneVkImage& target, VkPipeline pipeline, VkPipelineLayout layout,
              VkDescriptorSet set, const void* push, uint32_t pushBytes);

    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;   // linear, clamp
    VkDescriptorSetLayout singleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout singleLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout_ = VK_NULL_HANDLE;
    VkPipeline bloomPipeline_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE;
    VkPipeline fxaaPipeline_ = VK_NULL_HANDLE;

    SceneVkImage bloomExtract_;   // half-res HDR
    SceneVkImage bloomBlur_;      // half-res HDR
    SceneVkImage preFxaa_;        // full-res LDR, the tonemapped image FXAA reads
};

}  // namespace bro::scene::vk
