#pragma once

// The cloud deck's passes (scene_clouds.h):
//
//   PassCloudsPrepare  the engine's noise (once), the cover's bake (when the
//                      clouds are set) and the shadow's levels, into the
//                      graphics stream after the atmosphere's view tables and
//                      before the sky opens the frame's first scope, so every
//                      lit surface after reads this frame's shadow;
//   PassClouds         the march, drawn over the opaque surfaces in the HDR
//                      scope from the depth snapshot, after the decals and
//                      SSR and before anything blended.
//
// Neither runs in a frame without clouds.

#include "scene/vulkan/frame/scene_pass.h"

namespace bro::scene::vk {

class PassCloudsPrepare final : public ScenePass {
public:
    const char* name() const override { return "clouds-prepare"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

class PassClouds final : public ScenePass {
public:
    const char* name() const override { return "clouds"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
