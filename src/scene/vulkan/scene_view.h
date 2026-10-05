#pragma once

// One camera as the GPU sees it. Every view the scene renders — the main
// camera, a reflection-probe face, a shadow caster's light — is built here
// once, and every pass reads its matrices from it rather than deriving its own.
//
// Projections arrive in bromath's convention (y up in clip space, [0,1] depth
// per scene/depth_policy.h). Vulkan's clip space has y pointing down, so the
// projection is flipped exactly once, in toVulkanClip(); proj, viewProj and
// the inverses below all already include it.
//
// The GPU draws CAMERA-RELATIVE: every position a shader sees has the eye
// subtracted (world - eye), and the camera block's matrices take such
// positions to view space. The subtraction happens on the CPU where the two
// large values cancel (eyeRelative() on a model matrix, a light position, a
// probe box), so a scene far from the origin keeps fp32 precision near the
// camera. CPU culling, sorting and picking stay in absolute world space
// (view, viewProj, frustum).

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

/// `world` with `eye` subtracted from its translation: the camera-relative
/// model matrix every draw pushes.
bromath::Mat4 eyeRelative(const bromath::Mat4& world, const bromath::Vec3& eye);

/// `m` composed with a translation by `offset` (m * T(offset)): a matrix that
/// takes positions relative to one origin, re-based to take positions
/// relative to another `offset` away.
bromath::Mat4 rebased(const bromath::Mat4& m, const bromath::Vec3& offset);

struct SceneView {
    // Absolute world space: CPU culling, sorting and picking.
    bromath::Mat4 view;
    bromath::Mat4 proj;          // Vulkan clip space (toVulkanClip applied)
    bromath::Mat4 viewProj;
    bromath::Frustum frustum;    // culling planes of viewProj
    // Camera-relative, what the GPU draws with: (world - eye) -> view / clip.
    bromath::Mat4 relView;
    bromath::Mat4 relViewProj;
    bromath::Mat4 invRelView;
    bromath::Mat4 invProj;
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

    /// The camera block of every lit pipeline (set 0): the camera-relative
    /// matrices and the absolute eye. Fog and wind come from the renderer's
    /// settings; a view that should have neither passes none.
    SceneCameraUniforms uniforms(const SceneRenderer* fog) const;

    /// `world` relative to this view's eye.
    bromath::Mat4 relative(const bromath::Mat4& world) const { return eyeRelative(world, eye); }
    bromath::Vec3 relative(const bromath::Vec3& p) const { return p - eye; }

    /// Camera forward / right / up in world space.
    bromath::Vec3 forward() const { return {-view.at(2, 0), -view.at(2, 1), -view.at(2, 2)}; }
    bromath::Vec3 right() const { return {view.at(0, 0), view.at(0, 1), view.at(0, 2)}; }
    bromath::Vec3 up() const { return {view.at(1, 0), view.at(1, 1), view.at(1, 2)}; }
};

}  // namespace bro::scene::vk
