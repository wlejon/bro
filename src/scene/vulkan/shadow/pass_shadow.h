#pragma once

// The shadow pages a frame draws (SceneRenderer's ShadowPlan; world/
// shadow_plan.h, world/shadow_pages.h): each view, a rect of one shadow
// map's pages and their border, gets its casters drawn depth-only into its
// rect of the scratch target (SceneShadowPages::scratch). Two kinds of view:
//  - an overlay (ShadowViewPlan::dynamic): cleared and drawn with what moves
//    alone (skinned meshes, custom vertex chunks, a caster changing;
//    swaying vegetation in the levels where its sway shows), and sampled
//    where it is, its shadow multiplied into the cached page's;
//  - a redrawn page: cleared and drawn with everything that holds still
//    (and what sways where its sway does not show), then copied into the
//    pool (PassShadowCopy), timed as the world service's span of shadow
//    pages.
// Static, instanced (batched too), skinned and branch-tube casters (scatter
// leaves only when they carry vegetation records), drawn by the shared
// caster drawer (scene_caster_drawer.h: the colour pass's sways and custom
// vertex chunks, alpha cutouts). Casters are tested against each view's
// light volume, never the camera's: an off-screen caster still shadows what
// is on screen.

#include "scene/vulkan/frame/scene_pass.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class PassShadow final : public ScenePass {
public:
    const char* name() const override { return "shadow"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}

private:
    /// View `index`'s casters (its rect's viewport).
    void drawView(SceneFrame& frame, int index);
};

}  // namespace bro::scene::vk
