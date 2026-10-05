#pragma once

// Copies of the HDR scope's results that later passes sample while the scope
// keeps drawing into the originals:
//
//   PassDepthSnapshot  the opaque depth -> SceneTargets::depthSnapshot, for
//                      SSAO, SSR, decals, soft particles and depth of field;
//                      copied only on a frame where one of them reads it.
//   PassColorSnapshot  the lit, ambient-occluded opaque colour ->
//                      SceneTargets::ssrSnapshot, which SSR reflects.

#include "scene/vulkan/scene_pass.h"

namespace bro::scene::vk {

class PassDepthSnapshot final : public ScenePass {
public:
    const char* name() const override { return "depth-snapshot"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

class PassColorSnapshot final : public ScenePass {
public:
    const char* name() const override { return "color-snapshot"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
