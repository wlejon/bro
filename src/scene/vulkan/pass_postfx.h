#pragma once

// The post chain, in the GL renderer's order:
//
//   PassPostFx     HDR to LDR: bloom (bright pass at half resolution and a
//                  separable Gaussian whose reach is the bloom strength),
//                  then tonemapping (exposure, operator, gamma) with the 3D
//                  colour-grading LUT folded in. Reads dofHdr when depth of
//                  field ran, else hdr.
//   (PassOverlay   the unlit meshes and gizmos, drawn over the tonemapped
//                  image — pass_mesh.h)
//   PassTiltShift  the miniature look: a half-resolution blur of the LDR
//                  frame mixed in outside a horizontal focus band, then
//                  saturation and contrast.
//   PassFxaa       FXAA, always last.
//
// Each writes SceneFrame::ldrResult for the next; whichever runs last writes
// targets.ldr, the frame's output.

#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene {
class SceneRenderer;
}

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
    /// tonemap.frag's push block.
    struct TonemapPush {
        float exposure;
        float gamma;
        float bloomIntensity;
        int32_t tonemapMode;   // SceneRenderer::ToneMap
        float lutAmount;
        float lutScale;
        float lutOffset;
        float pad;
    };

    static SceneVkImage& source(const SceneFrame& frame);
    static SceneVkImage& output(const SceneFrame& frame);
    /// Upload the renderer's LUT when it changed; true when one is held.
    bool ensureLut(SceneGpu& gpu, const SceneRenderer& renderer);
    void bloom(SceneFrame& frame, const SceneVkImage& hdr);

    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;   // linear, clamp
    VkDescriptorSetLayout singleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout brightLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout_ = VK_NULL_HANDLE;
    VkPipeline brightPipeline_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE;

    SceneVkImage bloom_[2];       // half-res HDR ping-pong
    SceneVkImage lut_;            // the grading LUT, or a 1³ stand-in
    int lutSize_ = 0;
    uint64_t lutGeneration_ = 0;
};

class PassTiltShift final : public ScenePass {
public:
    const char* name() const override { return "tilt-shift"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout singleSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout compositeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blurLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipeline blurPipeline_ = VK_NULL_HANDLE;
    VkPipeline compositePipeline_ = VK_NULL_HANDLE;

    SceneVkImage blur_[2];   // half-res LDR ping-pong
    SceneVkImage out_;       // the composite when FXAA follows
};

class PassFxaa final : public ScenePass {
public:
    const char* name() const override { return "fxaa"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    SceneVkDevice* device_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace bro::scene::vk
