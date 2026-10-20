#pragma once

// PassHiZ: SceneTargets::hiZ, the pyramid of farDepth (the depth drawn so
// far, each pixel's farthest sample), built on the graphics queue right
// after each PassFarDepth, a compute dispatch a level (hiz_build.comp). A
// draw tests a shape's screen rect and nearest depth against it
// (scene_occlusion.glsl) and skips what all the depth drawn under it hides:
// the forest's sprite cards do in their vertex stage, so a card behind the
// opaque batches before it rasterises nothing, where PassFarDepth's
// per-pixel test alone ran its fragments up to the test.
//
// The pyramid is cleared to the far plane with farDepth (PassForestBatches),
// so the first batch culls nothing; the frame's own camera says it may cull
// (SceneCameraUniforms::occlusion), a probe's face never. BRO_SCENE_HIZ=0
// (read once) leaves it unbuilt and the cards untested, for A/B.

#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace bro::scene::vk {

struct OpaqueBatches;

/// Whether the frame builds and tests against the HiZ (BRO_SCENE_HIZ).
bool hiZEnabled();

class PassHiZ final : public ScenePass {
public:
    PassHiZ(const OpaqueBatches& batches, int batch) : batches_(batches), batch_(batch) {}
    const char* name() const override { return "hiZ"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    void destroyViews();

    const OpaqueBatches& batches_;
    int batch_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImage viewsOf_ = VK_NULL_HANDLE;   // the image levelViews_ are of
    std::vector<VkImageView> levelViews_;
};

}  // namespace bro::scene::vk
