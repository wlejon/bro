#pragma once

// The turf's surface layers (docs/scene-world-architecture.md §5): the tiles
// this frame fills and the page table's changes
// (SceneMeshDrawer::recordGroundLayers), outside any rendering, after the
// frame's uniforms and before anything draws the turf (the probe's faces
// and the opaque meshes).

#include "scene/vulkan/scene_pass.h"

namespace bro::scene::vk {

class PassGroundLayers final : public ScenePass {
public:
    const char* name() const override { return "ground-layers"; }
    bool setup(SceneGpu&) override { return true; }
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
