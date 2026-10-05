#pragma once

// The lighting the lit passes share: the camera and lighting uniform blocks
// and the sets that carry them.
//
// sceneLighting() fills the CPU half before any pass runs — the sun (or the
// implicit one), up to 16 other lights, the ambient and the shadow
// projection — so the shadow pass can render with it. PassFrameUniforms then
// adds what only exists once the frame is under way (the captured reflection
// probe, the tile shade map) and writes this frame's camera and lighting sets.

#include "scene/vulkan/scene_pass.h"
#include "scene/vulkan/scene_vk_descriptors.h"

namespace bro::scene {
class SceneGraph;
class SceneRenderer;
}

namespace bro::scene::vk {

struct SceneView;

/// Sun, lights, ambient and the directional shadow projection for `view`.
/// `shadowed` says whether the sun casts a shadow this frame.
SceneLightingUniforms sceneLighting(const SceneGraph& graph, const SceneRenderer& renderer,
                                    const SceneView& view, bool& shadowed);

/// A lighting set: `uniforms`, the shadow map array, a probe cube and a
/// shade map (fallbacks where `probeView` / `shadeView` are null).
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
