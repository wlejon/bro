#pragma once

// Colour grading: postLdr through the renderer's 3D LUT (trilinear, mixed
// in by the LUT amount) into ldr. The LUT is uploaded as a 3D texture
// whenever its generation moves; the renderer grades a frame only when
// ensureLut() has one (SceneFrame::lut), since the pass then owns ldr.

#include "scene/vulkan/scene_pass.h"

namespace bro::scene {
class SceneRenderer;
}

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class PassColorLut final : public ScenePass {
public:
    const char* name() const override { return "color-lut"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

    /// Upload the renderer's LUT if it changed (frame begun); true when one is held.
    bool ensureLut(SceneGpu& gpu, const SceneRenderer& renderer);

private:
    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    SceneVkImage lut_;
    int lutSize_ = 0;
    uint64_t lutGeneration_ = 0;
};

}  // namespace bro::scene::vk
