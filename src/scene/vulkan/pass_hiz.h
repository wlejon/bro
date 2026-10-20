#pragma once

// PassHiZ: SceneTargets::hiZ, the pyramid of the opaque batches' depth
// (farDepth, each pixel's farthest sample, PassFarDepth after the last
// batch), built on the graphics queue a compute dispatch a level
// (hiz_build.comp), with the view it was built from (SceneTargets::
// hiZState). A draw tests a shape's screen rect and nearest depth against
// it (scene_occlusion.glsl) and skips what all the depth drawn under it
// hides, in two phases: the batches hold back what the last frame's
// pyramid (hiZPrev, reprojected) hides and list it (SceneTargets::
// occlusionList, occlusion_list_layout.h); after building this frame's,
// PassHiZ tests the list against it and gathers what it does not hide into
// a draw a slot, which PassOpaque's retest draws. The forest's sprite cards
// do so in their vertex stage, so a hidden card rasterises nothing, where
// PassFarDepth's per-pixel test alone ran its fragments up to the test, and
// the retest runs only the cards it draws.
//
// The frame's own camera says it may cull (SceneCameraUniforms::occlusion),
// a probe's face never.

#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

#include <vector>

namespace bro::scene::vk {

struct OpaqueBatches;
struct SceneVkImage;

/// Whether this frame's batches hold cards back and the retest draws them:
/// the last frame's pyramid was built and a sprite draws.
bool occlusionRetests(const SceneFrame& frame, const OpaqueBatches& batches);

/// occlusion_list.comp's pipeline: binding 0 a pyramid (sampled), binding 1
/// SceneTargets::occlusionList, a stage a dispatch.
struct OcclusionListPipeline {
    /// The budget's slots' draws (occlusion_list_layout.h's OlDraws).
    struct OlDraws {
        struct U4 {
            uint32_t x, y, z, w;
        } v[255];
    };

    bool setup(SceneGpu& gpu);
    void cleanup();
    /// One dispatch of `stage` (OL_STAGE_*), testing against `hiZ` in `layout`;
    /// indirect (the list's OL_DISPATCH) for the test. The budget reads
    /// `draws` and the planners' buffers `planned` (OL_PLANNED, null unused).
    void dispatch(SceneFrame& frame, int stage, const SceneVkImage& hiZ, VkImageLayout layout,
                  const OlDraws* draws = nullptr, const VkBuffer* planned = nullptr) const;

    VkDevice device = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

/// At the batches' start: clears the occlusion list's head, takes each
/// slot's draw (OpaqueBatches::Draw::slot) as its command, the GPU
/// planner's copied, and enables the slots whose cards fit in the list.
class PassOcclusionBudget final : public ScenePass {
public:
    explicit PassOcclusionBudget(const OpaqueBatches& batches) : batches_(batches) {}
    const char* name() const override { return "occlusionBudget"; }
    bool setup(SceneGpu& gpu) override { return pipeline_.setup(gpu); }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override { pipeline_.cleanup(); }

private:
    const OpaqueBatches& batches_;
    OcclusionListPipeline pipeline_;
};

class PassHiZ final : public ScenePass {
public:
    explicit PassHiZ(const OpaqueBatches& batches) : batches_(batches) {}
    const char* name() const override { return "hiZ"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    void destroyViews();
    /// The level views of the pyramid `image` (one of the two), else null.
    const std::vector<VkImageView>* viewsOf(VkImage image) const;

    const OpaqueBatches& batches_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    OcclusionListPipeline list_;
    // A storage view a level of each pyramid (they swap each frame).
    VkImage viewsOf_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    std::vector<VkImageView> levelViews_[2];
};

}  // namespace bro::scene::vk
