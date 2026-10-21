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
// leaves only when they carry vegetation records), drawn by the shared
// caster drawer (scene_caster_drawer.h: the colour pass's sways and custom
// vertex chunks, alpha cutouts). Casters are tested against each view's
// light volume, never the camera's: an off-screen caster still shadows what
// is on screen.
//
// With async compute the pass runs as two: the overlays' casters that
// read nothing of the frame's compute (Part::Unplanned), beside the plan,
// then, after the frame's join, those a GPU plan drew up (Part::Planned: the
// plants' indirect draws) and every redrawn page, timed as the world
// service's span of shadow pages. A shadow map is depth only, its texel the
// nearest of its casters', so the order of the casters leaves it the same.

#include "scene/vulkan/frame/scene_pass.h"

#include <vulkan/vulkan.h>

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
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    /// View `index`'s casters (its rect's viewport): the planned ones, the
    /// unplanned ones, or both.
    void drawView(SceneFrame& frame, int index, bool planned, bool unplanned);

    Part part_;
};

}  // namespace bro::scene::vk
