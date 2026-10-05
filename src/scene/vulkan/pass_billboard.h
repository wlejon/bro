#pragma once

// World-anchored billboards: every node with a world anchor (shapes, sprites,
// HTML panels) as a camera-facing quad — Y-locked when the node asks — and,
// with light icons on, a marker per light. Premultiplied over the HDR scope,
// depth-tested against the scene, after the particles.

#include "scene/vulkan/scene_gpu_resources.h"
#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene {
struct NodeTexture;
class SceneNode;
class LightNode;
}

namespace bro::scene::vk {

class PassBillboard final : public ScenePass {
public:
    const char* name() const override { return "billboards"; }
    bool setup(SceneGpu& gpu) override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// billboard.vert/frag's push block.
    struct alignas(16) Push {
        float anchor[4];     // xyz world anchor
        float right[4];      // xyz quad right
        float up[4];         // xyz quad up
        float halfSize[2];
        float uvMin[2];
        float uvMax[2];
        float strokeWidth;   // in uv units
        int32_t shapeMode;   // 0 rect, 1 circle, 2 premultiplied texture (HTML), 3 ring, 4 straight texture (sprite)
        float color[4];
        float stroke[4];
    };

    /// Fill `push` (and the texture's set) for an anchored node; false when
    /// it draws nothing.
    bool prepareNode(SceneFrame& frame, SceneNode& node, Push& push, VkDescriptorSet& material);
    void prepareLightIcon(const SceneFrame& frame, const LightNode& light, Push& push);
    VkDescriptorSet textureSet(SceneFrame& frame, const NodeTexture& tex, uint32_t nodeId, TextureSlot slot);

    SceneVkDevice* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
