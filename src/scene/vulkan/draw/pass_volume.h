#pragma once

// Volumes (scene.createVolume): each a triangle over the whole view whose
// custom chunk marches the pixel's ray through a medium up to the opaque
// depth (draw/volume.frag). They draw, in tree order, into a target of their
// own at the frame's size: light over transmittance (each volume over the
// ones before it, dst * transmittance + its light) and the distance the
// medium's light comes from. A march samples a few points of a long ray, and
// a per-pixel offset of its steps (camera.frame.x changes it every render)
// turns their edges into grain; the grain is averaged over renders, the way
// cloud renderers reconstruct a jittered march (Schneider's Nubis, Hillaire's
// Frostbite clouds): last render's result is found again where the medium's
// depth reprojects the pixel to, kept within the range of the pixel's
// neighbourhood this render (what moved or appeared is not smeared from
// where it was) and blended with this render's at kBlend. A render that does
// not follow the last one (the first, a resize, a render without volumes
// before it, another set of volumes) takes this render's alone. The mesh drawer builds and binds the
// volumes (SceneMeshDrawer::recordVolume), so a volume's uniforms, buffers
// and textures are a custom shader's.
//
// PassVolumeComposite then blends the result over the HDR scope as scene *
// transmittance + light: after the opaque surfaces, their sky and the
// decals, before SSR and the translucent surfaces.

#include "scene/vulkan/frame/scene_pass.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <bromath/mat.h>
#include <bromath/vec.h>

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene::vk {

class PassVolume final : public ScenePass {
public:
    /// This render's share of the result where last render's is found.
    static constexpr float kBlend = 0.1f;

    const char* name() const override { return "volumes"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

    /// The reconstructed volumes of the render recorded last (light,
    /// transmittance), in SHADER_READ_ONLY_OPTIMAL.
    const SceneVkImage& result() const { return history_[current_]; }
    VkSampler sampler() const { return sampler_; }

private:
    /// volume_resolve.frag's uniform block.
    struct Uniforms {
        float prevViewProj[16];   // last render's camera-relative clip
        float invViewProj[16];    // this render's, inverted
        float eyeDelta[4];        // xyz: this render's eye - last render's; w: this render's share
    };

    SceneVkDevice* device_ = nullptr;
    SceneVkImage medium_, mediumDepth_;
    SceneVkImage history_[2];
    uint32_t current_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline resolve_ = VK_NULL_HANDLE;

    // The render the history is of (its index, its volumes' ids hashed, its
    // camera); none after a resize.
    bool haveHistory_ = false;
    uint32_t lastIndex_ = 0;
    uint64_t lastVolumes_ = 0;
    bromath::Mat4 prevViewProj_;
    bromath::Vec3 prevEye_{0.0f, 0.0f, 0.0f};
};

class PassVolumeComposite final : public ScenePass {
public:
    explicit PassVolumeComposite(const PassVolume& volumes) : volumes_(volumes) {}

    const char* name() const override { return "volumes composite"; }
    bool setup(SceneGpu& gpu) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

private:
    const PassVolume& volumes_;
    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkShaderModule fs_ = VK_NULL_HANDLE;
    PipelineVariants pipelines_;
};

}  // namespace bro::scene::vk
