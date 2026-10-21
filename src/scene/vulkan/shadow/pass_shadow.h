#pragma once

// The shadow pages a frame draws (SceneRenderer's ShadowPlan; world/
// shadow_plan.h, world/shadow_pages.h): each view, a rect of one shadow
// map's pages and their border, gets its casters drawn depth-only into its
// rect of the scratch target (SceneShadowPages::scratch). Two kinds of view:
//  - an overlay (ShadowViewPlan::dynamic): cleared and drawn with what moves
//    alone (skinned meshes, custom vertex chunks, a caster changing;
//    swaying vegetation and the plants in the levels where their sway
//    shows; the grass), and sampled where it is, its shadow multiplied
//    into the cached page's;
//  - a redrawn page: cleared and drawn with everything that holds still
//    (and what sways where its sway does not show), then copied into the
//    pool (PassShadowCopy).
// Static, instanced (batched too), skinned and branch-tube casters (scatter
// leaves only when they carry vegetation records), with the colour pass's
// wind sway, vegetation sway and custom vertex chunks so the silhouette
// matches what is drawn. A textured static or instanced mesh with an alpha
// cutoff casts its cut (its base colour texture at set 2). Casters are
// tested against each view's light volume, never the camera's: an
// off-screen caster still shadows what is on screen.
//
// With async compute the pass runs as two: the overlays' casters that
// read nothing of the frame's compute (Part::Unplanned), beside the plan,
// then, after the frame's join, those a GPU plan drew up (Part::Planned: the
// plants' indirect draws) and every redrawn page, timed as the world
// service's span of shadow pages. A shadow map is depth only, its texel the
// nearest of its casters', so the order of the casters leaves it the same.

#include "scene/vulkan/draw/scene_mesh_drawer.h"
#include "scene/vulkan/frame/scene_pass.h"

#include <vulkan/vulkan.h>

#include <string>
#include <unordered_map>

namespace bro::scene::vk {

class PassShadow final : public ScenePass {
public:
    enum class Part : uint8_t {
        All,         // every caster (no async compute)
        Unplanned,   // the overlays, cleared, and their casters that read no compute results
        Planned,     // the overlays' GPU-planned casters (MeshDraw::indirect) and the redrawn pages, after the join
    };
    explicit PassShadow(Part part = Part::All) : part_(part) {}
    const char* name() const override { return part_ == Part::Planned ? "shadowPlanned" : "shadow"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    /// shaders/common/scene_shadow_push.glsl.
    struct alignas(16) Push {
        float lightViewProj[16];
        float modelRows[12];
        float params[4];   // x wind mask (a plants caster's mode); yzw a cutout caster's
    };
    static_assert(sizeof(Push) == 128);

    VkPipeline builtinPipeline(MeshKind kind, bool twoSided);
    VkPipeline cutoutPipeline(MeshKind kind, bool twoSided);
    VkPipeline customPipeline(const CustomShaderState& cs, MeshKind kind, bool twoSided, bool cutout);
    VkPipeline buildPipeline(VkShaderModule vs, MeshKind kind, bool custom, bool twoSided, bool cutout);
    void drawCaster(SceneFrame& frame, const bromath::Mat4& lightViewProj, const MeshDraw& draw,
                    const bromath::Frustum* tileFrustum);
    /// View `index`'s casters (its rect's viewport): the planned ones, the
    /// unplanned ones, or both.
    void drawView(SceneFrame& frame, int index, bool planned, bool unplanned);

    Part part_;
    SceneVkDevice* device_ = nullptr;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_[kMeshKindCount] = {};
    VkShaderModule fs_ = VK_NULL_HANDLE;
    VkPipeline builtin_[kMeshKindCount][2] = {};   // [kind][two-sided]
    // Cutout casters (a textured static or instanced mesh with an alpha
    // cutoff): their base colour's alpha cut, scene_shadow_cutout.glsl.
    VkShaderModule vsCutout_[2] = {};              // [instanced]
    VkShaderModule fsCutout_ = VK_NULL_HANDLE;
    VkPipeline cutout_[2][2] = {};                 // [instanced][two-sided]
    // Custom-vertex casters by (chunk key, kind, sidedness); VK_NULL_HANDLE
    // remembers a failed compile, so it is reported once and the caster
    // falls back to its undisplaced silhouette.
    std::unordered_map<std::string, VkPipeline> custom_;
};

}  // namespace bro::scene::vk
