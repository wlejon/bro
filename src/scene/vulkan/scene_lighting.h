#pragma once

// The lighting the lit passes share: the camera and lighting uniform blocks
// and the sets that carry them.
//
// sceneLighting() fills the CPU half before any pass runs — SceneRenderer's
// light list (up to 32, the implicit sun when the scene has none), the
// ambient and the shadow plan's atlas tiles. writeCameraSet() writes the
// camera set then too, since the shadow pass draws with it. PassFrameUniforms
// adds what only exists once the frame is under way (the captured reflection
// probe, the tile shade map) and writes this frame's lighting set.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_descriptors.h"

namespace bro::scene {
class SceneRenderer;
}

namespace bro::scene::vk {

/// The lights, ambient and shadow tiles of `renderer`'s current frame
/// (SceneRenderer::frameLights / shadowPlan).
SceneLightingUniforms sceneLighting(const SceneRenderer& renderer);

/// The frame's camera set (frame.cameraSet), from frame.view.
void writeCameraSet(SceneFrame& frame);

/// A lighting set: `uniforms`, the shadow atlas, a probe cube and a shade
/// map (fallbacks where `probeView` / `shadeMap` are null).
VkDescriptorSet writeLightingSet(SceneGpu& gpu, const SceneLightingUniforms& uniforms, VkImageView probeView,
                                 const SceneVkImage* shadeMap);

class PassFrameUniforms final : public ScenePass {
public:
    const char* name() const override { return "frame-uniforms"; }
    bool setup(SceneGpu&) override { return true; }
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
