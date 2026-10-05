#pragma once

// The sky: a full-screen procedural gradient (with the sun) drawn at the far
// plane into the HDR scope, behind everything the opaque passes draw. Runs
// when the atmosphere is enabled or an environment map is set. The cube
// binding is the IBL environment's slot; until IBL is ported it samples the
// shared black fallback cube and the shader's procedural path is what shows.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassEnvironment final : public ScenePass {
public:
    const char* name() const override { return "sky"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// environment.frag's push block (128 bytes).
    struct alignas(16) Push {
        float invViewProj[16];   // screen -> world ray
        float sunDir[4];         // xyz sun direction, w = has environment cube
        float skyColor[4];       // rgb, a = sky intensity
        float horizonColor[4];   // rgb, a = sun size
        float groundColor[4];    // rgb, a = sun intensity
    };

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout cubeLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
