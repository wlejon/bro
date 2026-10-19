#pragma once

// The shadow atlas: every tile SceneRenderer's ShadowPlan marks for render
// (directional cascades, spot cones, point-light cube faces; scene/
// shadow_plan.h) gets its casters drawn depth-only into its rect of
// SceneTargets::shadowAtlas. Cached tiles keep the depth an earlier frame
// drew. Static, instanced (batched too), skinned and branch-tube casters
// (scatter leaves only when they carry vegetation records), with the colour
// pass's wind sway, vegetation sway and custom vertex chunks so the
// silhouette matches what is drawn.
// Casters are tested against each tile's light volume, never the camera's:
// an off-screen caster still shadows what is on screen.
//
// With async compute the pass runs as two: the casters that read nothing of
// the frame's compute (Part::Unplanned, which clears the tiles), then, after
// the frame's join, those a GPU plan drew up (Part::Planned: a forest's
// indirect draws), so the first runs beside the plan. A shadow map is depth
// only, its texel the nearest of its casters', so the order of the casters
// leaves it the same.

#include "scene/vulkan/scene_mesh_drawer.h"
#include "scene/vulkan/scene_pass.h"

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

namespace bro::scene::vk {

class PassShadow final : public ScenePass {
public:
    enum class Part : uint8_t {
        All,         // every caster (no async compute)
        Unplanned,   // the casters that read no compute results; clears the tiles
        Planned,     // the GPU-planned casters (MeshDraw::indirect), after the join
    };
    explicit PassShadow(Part part = Part::All) : part_(part) {}
    const char* name() const override { return part_ == Part::Planned ? "shadowPlanned" : "shadow"; }
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
        float params[4];   // x wind mask; yzw a forest caster's near sphere
    };
    static_assert(sizeof(Push) == 128);

    VkPipeline builtinPipeline(MeshKind kind, bool twoSided, bool leafInline);
    VkPipeline customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided);
    VkPipeline buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided);
    void drawCaster(SceneFrame& frame, const bromath::Mat4& lightViewProj, const MeshDraw& draw,
                    const bromath::Frustum* tileFrustum);

    Part part_;
    SceneVkDevice* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[kMeshKindCount] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkShaderModule fsForest_ = VK_NULL_HANDLE;   // forest sprites' leafy cutout
    VkPipeline builtin_[kMeshKindCount][2] = {};   // [kind][two-sided]
    // Forest leaf casters past the pose buffer's room (kForestLeafInline),
    // each vertex posing its leaf: shadow_forest_leaf.vert's inline build.
    VkShaderModule vsLeafInline_ = VK_NULL_HANDLE;
    VkPipeline leafInline_[2] = {};                // [two-sided]
    // Custom-vertex casters by (chunk key, kind, sidedness); VK_NULL_HANDLE
    // remembers a failed compile, so it is reported once and the caster
    // falls back to its undisplaced silhouette.
    std::unordered_map<std::string, VkPipeline> custom_;
};

}  // namespace bro::scene::vk
