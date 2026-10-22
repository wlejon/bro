#pragma once

// The lighting the lit passes share: the camera and lighting uniform blocks
// and the sets that carry them.
//
// sceneLighting() fills the CPU half before any pass runs — SceneRenderer's
// light list (up to 32, the implicit sun when the scene has none), the
// ambient, the shadow plan's maps, the environment's image-based
// lighting and the atmosphere. writeCameraSet() writes the
// camera set then too, since the shadow pass draws with it. PassFrameUniforms
// writes this frame's lighting set and gives every draw sampling a tile shade
// map a set carrying it; the reflection-probe pass gives each draw inside a
// probe's box one carrying the probe (and the draw's shade map).

#include "scene/vulkan/frame/scene_pass.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"
#include "scene/vulkan/frame/scene_view.h"

#include <bromath/vec.h>

namespace bro::scene {
class ReflectionProbeNode;
class SceneRenderer;
struct ShadowPlan;
struct ShadowMapPlan;
struct ShadeMapBinding;
}

namespace bro::scene::vk {

/// The lights, ambient and shadow maps of `renderer`'s current frame
/// (SceneRenderer::frameLights / shadowPlan), with the environment's IBL, the
/// atmosphere and the probe grid (gpu.environment, gpu.atmosphere,
/// gpu.probes, as they began the frame), for `view`: every position is
/// relative to its eye (camera-relative rendering, scene_view.h), the
/// lights' from the place frame's, what is keyed on the world (the shadow
/// pages, the atmosphere, the probe grid, the land's sun) from the body's.
SceneLightingUniforms sceneLighting(const SceneRenderer& renderer, const SceneGpu& gpu, const SceneView& view);

/// The sun's penumbra as the lit shaders read it (SceneLightingUniforms::
/// shadowSoft): its width a metre along the light, the still penumbra, the
/// search level (-1: none).
void shadowSoftUniform(const SceneRenderer& renderer, float out[4]);

/// Shadow map `map` of `plan`, for positions relative to `eye`.
void shadowMapUniform(const ShadowPlan& plan, const ShadowMapPlan& map, const bromath::Vec3& eye,
                      SceneShadowMapUniform& out);

/// The frame's camera set (frame.cameraSet), from frame.view.
void writeCameraSet(SceneFrame& frame);

/// A lighting set: `uniforms`, the shadow pages (their pool and page table),
/// a probe cube, a shade map (fallbacks where `probeView` / `shadeMap` are
/// null) and the environment.
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
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu&) override {}
};

}  // namespace bro::scene::vk
