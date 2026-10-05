#pragma once

// The shadow atlas: every tile SceneRenderer's ShadowPlan marks for render
// (directional cascades, spot cones, point-light cube faces; scene/
// shadow_plan.h) gets its casters drawn depth-only into its rect of
// SceneTargets::shadowAtlas. Cached tiles keep the depth an earlier frame
// drew. Static, instanced and skinned casters, with the colour pass's wind
// sway and custom vertex chunks so the silhouette matches what is drawn.
// Casters are tested against each tile's light volume, never the camera's:
// an off-screen caster still shadows what is on screen.

#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

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
    /// shaders/scene_shadow_push.glsl.
    struct alignas(16) Push {
        float lightViewProj[16];
        float modelRows[12];
        float params[4];   // x wind mask
    };
    static_assert(sizeof(Push) == 128);

    VkPipeline builtinPipeline(MeshKind kind, bool twoSided);
    VkPipeline customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided);
    VkPipeline buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided);
    void drawCaster(SceneFrame& frame, const bromath::Mat4& lightViewProj, const MeshDraw& draw);

    SceneVkDevice* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[3] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkPipeline builtin_[3][2] = {};   // [kind][two-sided]
    // Custom-vertex casters by (chunk key, kind, sidedness); VK_NULL_HANDLE
    // remembers a failed compile, so it is reported once and the caster
    // falls back to its undisplaced silhouette.
    std::unordered_map<std::string, VkPipeline> custom_;
};

}  // namespace bro::scene::vk
