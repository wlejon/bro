#pragma once

// The lighting the lit passes share: the camera and lighting uniform blocks
// and the sets that carry them.
//
// sceneLighting() fills the CPU half before any pass runs — SceneRenderer's
// light list (up to 32, the implicit sun when the scene has none), the
// ambient, the shadow plan's atlas tiles, the environment's image-based
// lighting and the atmosphere. writeCameraSet() writes the
// camera set then too, since the shadow pass draws with it. PassFrameUniforms
// writes this frame's lighting set and gives every draw sampling a tile shade
// map a set carrying it; the reflection-probe pass gives each draw inside a
// probe's box one carrying the probe (and the draw's shade map).

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_descriptors.h"

#include <bromath/vec.h>

namespace bro::scene {
class ReflectionProbeNode;
class SceneRenderer;
struct ShadeMapBinding;
}

namespace bro::scene::vk {

class SceneEnvironment;

/// The lights, ambient and shadow tiles of `renderer`'s current frame
/// (SceneRenderer::frameLights / shadowPlan), with `environment`'s IBL and
/// the atmosphere, for a view whose eye is `eye`: every position is relative
/// to it (camera-relative rendering, scene_view.h).
SceneLightingUniforms sceneLighting(const SceneRenderer& renderer, const SceneEnvironment& environment,
                                    const bromath::Vec3& eye);

/// The frame's camera set (frame.cameraSet), from frame.view.
void writeCameraSet(SceneFrame& frame);

/// A lighting set: `uniforms`, the shadow atlas, a probe cube, a shade map
/// (fallbacks where `probeView` / `shadeMap` are null) and the environment.
VkDescriptorSet writeLightingSet(SceneGpu& gpu, const SceneLightingUniforms& uniforms, VkImageView probeView,
                                 const SceneVkImage* shadeMap);

/// A captured reflection probe, as a lighting set samples it.
struct ProbeLighting {
    const ReflectionProbeNode* node = nullptr;
    VkImageView view = VK_NULL_HANDLE;   // the prefiltered cube
    uint32_t mipLevels = 0;
};

/// The lighting set of a draw lit through `probe` and shaded by `shade`
/// (either may be null): the frame's own when both are, else one written
/// once per (probe, shade map) pair per frame.
VkDescriptorSet lightingSetFor(SceneFrame& frame, const ProbeLighting* probe, const ShadeMapBinding* shade);

/// `base` with `shade`'s map (origin relative to `eye`), for a view other
/// than the frame's own (a reflection-probe face).
VkDescriptorSet shadedLightingSet(SceneGpu& gpu, const SceneLightingUniforms& base, const ShadeMapBinding& shade,
                                  const bromath::Vec3& eye);

class PassFrameUniforms final : public ScenePass {
public:
    const char* name() const override { return "frame-uniforms"; }
    bool setup(SceneGpu&) override { return true; }
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
