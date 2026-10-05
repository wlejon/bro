#pragma once

// The sky, drawn into the HDR scope behind everything the opaque passes
// draw: the atmosphere, else the environment's radiance cube, then the
// starfield (SceneEnvironment::drawSky). Perspective cameras only.

#include "scene/vulkan/scene_pass.h"

namespace bro::scene::vk {

class PassEnvironment final : public ScenePass {
public:
    const char* name() const override { return "sky"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
