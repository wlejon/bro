#pragma once

// 3D particle systems: camera-facing quads, one instance per live particle
// (sorted back to front by the node), premultiplied-over or additive, soft
// against the opaque depth snapshot. Drawn in the HDR scope after the
// translucent meshes; depth-tested, never depth-writing.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassParticles final : public ScenePass {
public:
    const char* name() const override { return "particles"; }
    bool setup(SceneGpu& gpu) override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// particles.vert/frag's push block.
    struct alignas(16) Push {
        float model[16];
        float camRight[4];
        float camUp[4];
        float params[4];   // sheet cols, rows, textured, softness distance
    };

    VkPipeline pipeline(const TargetFormat& target, bool additive);

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;   // texture, scene depth
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
