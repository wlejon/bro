#pragma once

// Clipmap terrain: the ring mesh a ClipmapTerrain owns (MeshNode with a
// clipmapRole), displaced on the GPU from its height array and shaded from
// its surface array, drawn into the HDR scope with the opaque meshes. The
// shaders are assembled from the clipmap GLSL sources per (cubic height,
// cubic surface) variant; the node's sampler slots and uniforms feed set 2.

#include "scene/mesh_node.h"
#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassTerrain final : public ScenePass {
public:
    const char* name() const override { return "terrain"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    VkPipeline pipeline(const MeshNode::ClipmapRole& role, const TargetFormat& target);
    /// This frame's copy of the node's terrain uniform block.
    static VkDescriptorBufferInfo uniforms(SceneVkDevice& device, const MeshNode* node);

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout terrainLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
    SceneVkImage emptyHeights_;
    SceneVkImage emptySurfaces_;
};

}  // namespace bro::scene::vk
