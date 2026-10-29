#pragma once

// The world service's frame (scene/world/world_service.h;
// SceneWorldService::runFrame): its producers' offers, the policy's grants
// and the jobs, at the frame's head, outside any rendering, after the
// frame's uniforms and before anything draws what the jobs make (the turf's
// surface layers: the probe's faces and the opaque meshes).

#include "scene/vulkan/frame/scene_pass.h"

namespace bro::scene::vk {

class PassWorldService final : public ScenePass {
public:
    const char* name() const override { return "world-service"; }
    bool setup(SceneGpu&) override { return true; }
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
