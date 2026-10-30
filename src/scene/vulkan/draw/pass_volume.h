#pragma once

// Volumes (scene.createVolume): each a triangle over the whole view whose
// custom chunk marches the pixel's ray through a medium up to the opaque
// depth (draw/volume.frag), blended as scene * transmittance + its light.
// Drawn in the HDR scope after the opaque surfaces, their sky and the decals,
// and before SSR and the translucent surfaces, in tree order. The mesh
// drawer builds and binds them (SceneMeshDrawer::recordVolume), so a
// volume's uniforms, buffers and textures are a custom shader's.

#include "scene/vulkan/frame/scene_pass.h"

namespace bro::scene::vk {

class PassVolume final : public ScenePass {
public:
    const char* name() const override { return "volumes"; }
    bool setup(SceneGpu&) override { return true; }
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
