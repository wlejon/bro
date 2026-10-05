#pragma once

// One camera as the GPU sees it. Every view the scene renders — the main
// camera, a reflection-probe face, a shadow caster's light — is built here
// once, and every pass reads its matrices from it rather than deriving its own.
//
// Projections arrive in bromath's convention (y up in clip space, [0,1] depth
// per scene/depth_policy.h). Vulkan's clip space has y pointing down, so the
// projection is flipped exactly once, in toVulkanClip(); proj, viewProj and
// the inverses below all already include it.

#include "scene/vulkan/scene_vk_descriptors.h"

#include <bromath/frustum.h>
#include <bromath/mat.h>
#include <bromath/vec.h>

#include <cstdint>

namespace bro::scene {
class SceneGraph;
class SceneRenderer;
}

namespace bro::scene::vk {

/// The one place y-up clip space becomes Vulkan's y-down clip space.
bromath::Mat4 toVulkanClip(const bromath::Mat4& proj);

struct SceneView {
    bromath::Mat4 view;
    bromath::Mat4 proj;          // Vulkan clip space (toVulkanClip applied)
    bromath::Mat4 viewProj;
    bromath::Mat4 invView;
    bromath::Mat4 invProj;
    bromath::Mat4 invViewProj;
    bromath::Frustum frustum;    // culling planes of viewProj
    bromath::Vec3 eye{0.0f, 0.0f, 0.0f};
    float nearZ = 0.1f;
    float farZ = 100.0f;
    bool perspective = true;
    uint32_t width = 0;
    uint32_t height = 0;

    /// `projection` in bromath's convention (scene/depth_policy.h builders).
    static SceneView make(const bromath::Mat4& view, const bromath::Mat4& projection,
                          const bromath::Vec3& eye, float nearZ, float farZ,
                          bool perspective, uint32_t width, uint32_t height);

    /// The graph's active camera rendered at `width` x `height`.
    static SceneView fromCamera(const SceneGraph& graph, uint32_t width, uint32_t height);

    /// The camera block of every lit pipeline (set 0). Fog comes from the
    /// renderer's settings; a view that should not be fogged passes none.
    SceneCameraUniforms uniforms(const SceneRenderer* fog) const;

    /// Camera forward / right / up in world space.
    bromath::Vec3 forward() const { return {-view.at(2, 0), -view.at(2, 1), -view.at(2, 2)}; }
    bromath::Vec3 right() const { return {view.at(0, 0), view.at(0, 1), view.at(0, 2)}; }
    bromath::Vec3 up() const { return {view.at(1, 0), view.at(1, 1), view.at(1, 2)}; }
};

}  // namespace bro::scene::vk
