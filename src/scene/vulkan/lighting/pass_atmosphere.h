#pragma once

// The atmosphere's view tables (scene_atmosphere.h): this frame's sky view
// (when the eye's height or the sun moved enough), into the graphics
// stream, and aerial-perspective froxels, into the frame's compute stream,
// after the world service (which remakes the view-independent tables they
// read) and before anything lit or the sky reads them.

#include "scene/vulkan/frame/scene_pass.h"

namespace bro::scene::vk {

class PassAtmosphere final : public ScenePass {
public:
    const char* name() const override { return "atmosphere"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
