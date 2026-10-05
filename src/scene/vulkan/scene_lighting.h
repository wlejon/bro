#pragma once

// The lighting the lit passes share: the camera and lighting uniform blocks
// and the sets that carry them.
//
// sceneLighting() fills the CPU half before any pass runs — SceneRenderer's
// light list (up to 32, the implicit sun when the scene has none), the
// ambient, the shadow plan's atlas tiles, the environment's image-based
// lighting and the atmosphere. writeCameraSet() writes the
// camera set then too, since the shadow pass draws with it. PassFrameUniforms
// adds what only exists once the frame is under way (the tile shade map) and
// writes this frame's lighting set; the reflection-probe pass writes one more
// per probe.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_descriptors.h"

namespace bro::scene {
class ReflectionProbeNode;
class SceneRenderer;
}

namespace bro::scene::vk {

class SceneEnvironment;

/// The lights, ambient and shadow tiles of `renderer`'s current frame
/// (SceneRenderer::frameLights / shadowPlan), with `environment`'s IBL and
/// the atmosphere.
SceneLightingUniforms sceneLighting(const SceneRenderer& renderer, const SceneEnvironment& environment);

/// The frame's camera set (frame.cameraSet), from frame.view.
void writeCameraSet(SceneFrame& frame);

/// A lighting set: `uniforms`, the shadow atlas, a probe cube, a shade map
/// (fallbacks where `probeView` / `shadeMap` are null) and the environment.
VkDescriptorSet writeLightingSet(SceneGpu& gpu, const SceneLightingUniforms& uniforms, VkImageView probeView,
                                 const SceneVkImage* shadeMap);

/// `probe`'s box, intensity and blend margin in `uniforms`, sampling a
/// prefiltered cube of `mipLevels` mips.
void setProbe(SceneLightingUniforms& uniforms, const ReflectionProbeNode& probe, uint32_t mipLevels);

class PassFrameUniforms final : public ScenePass {
public:
    const char* name() const override { return "frame-uniforms"; }
    bool setup(SceneGpu&) override { return true; }
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
