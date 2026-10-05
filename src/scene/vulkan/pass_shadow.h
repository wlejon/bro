#pragma once

// The directional shadow map: the sun's casters drawn depth-only into
// cascade 0 of SceneTargets::shadow with the projection sceneLighting()
// computed (scene_lighting.h). Static, instanced and skinned casters,
// including custom shaders whose vertex chunk displaces the silhouette.
// Casters are drawn whether or not the camera sees them.

#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace bro::scene::vk {

class PassShadow final : public ScenePass {
public:
    const char* name() const override { return "shadow"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    struct alignas(16) Push {
        float lightMVP[16];
    };

    VkPipeline builtinPipeline(MeshKind kind);
    VkPipeline customPipeline(const CustomShaderState& cs, MeshKind kind);
    VkPipeline buildPipeline(VkShaderModule vs, MeshKind kind, bool custom);

    SceneVkDevice* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[3] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkPipeline builtin_[3] = {};
    // Custom-vertex casters by (chunk key, kind); VK_NULL_HANDLE remembers a
    // failed compile so it is reported once.
    std::unordered_map<std::string, VkPipeline> custom_;
};

}  // namespace bro::scene::vk
