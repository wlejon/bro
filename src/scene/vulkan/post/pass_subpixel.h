#pragma once

// The sub-pixel resolve: what grows on the ground far off is detail finer
// than a pixel (a far fan's leaves, a herb's leaves and blooms a few pixels
// across, the cover's far tier), and a cut card keeps such a pixel all or
// nothing by its dither (common/scene_cover_mask.glsl). Still, the pixels
// average to the right cover; moving, they flip between what the cards show
// and what is behind them, frame to frame. So after the opaque surfaces,
// each such pixel is blended with its neighbours: a 3x3 tent,
// deterministic, over the neighbours at its own depth only (a trunk, a
// hill's crest or the sky beyond is not smeared into it).
//
// The opaque scope's draws write the mark (SceneTargets::mark,
// common/scene_subpixel_mark.glsl): the ground the cards stand on, which
// blends as far as its most marked neighbour, or a surface of its own with
// the share of it that was the dither's decision on such detail, which
// blends as far as that (a solid leaf, blade or trunk: none). Then:
//
//   PassSubpixelSnapshot  copies the opaque colour to colorSnapshot
//                         (frame/pass_snapshot.h);
//   PassSubpixel          draws the blend full-screen in the HDR scope from
//                         it, the mark and the depth snapshot, after AO is
//                         applied and before decals, SSR and the
//                         translucents, so nothing drawn later is blurred.

#include "scene/vulkan/frame/scene_pass.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class SceneVkDevice;

class PassSubpixel final : public ScenePass {
public:
    const char* name() const override { return "subpixel"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
