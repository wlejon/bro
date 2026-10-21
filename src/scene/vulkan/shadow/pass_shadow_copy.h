#pragma once

// The pages the shadow pass redrew, copied from the scratch target into the
// pool (SceneShadowPages; ShadowPlan::copyOut): each into its physical page,
// border and all, ahead of the passes that sample it. The overlays stay in
// the scratch target, where the lit shaders sample them.

#include "scene/vulkan/frame/scene_pass.h"

namespace bro::scene::vk {

class PassShadowCopy final : public ScenePass {
public:
    const char* name() const override { return "shadow:copy"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
