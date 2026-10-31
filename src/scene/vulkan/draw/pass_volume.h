#pragma once

// Volumes (scene.createVolume): each a triangle over the whole view whose
// custom module marches the pixel's ray through a medium up to the opaque
// depth (draw/volume_main.bk). A render marches a quarter of the frame's
// pixels, one of each 2 x 2 block, the four in turn (volume::interleave), as
// Schneider's Nubis clouds do: they draw, in tree order, into a target of
// their own at half the frame's size, a texel for each block's pixel at its
// own ray (a viewport moved by a fraction of a texel: volume::frame_pixel):
// light over transmittance (each volume over the ones before it, dst *
// transmittance + its light) and the distance the medium's light comes from.
// A march samples a few points of a long ray, and a per-pixel offset of its
// steps (volume::march_index changes it every march) turns their edges into
// grain. The resolve rebuilds the frame at its full size from the marches
// and the last render's result, found again where the medium's depth
// reprojects the pixel to and kept within the range of the marches about
// the pixel this render (what moved or appeared is not smeared from where it
// was): a pixel marched this render blends its march in at kBlend, which
// averages the grain over its marches; the others keep what was found. A
// pixel no history covers (the first render, a resize, a render without
// volumes before it, another set of volumes, what came into view) takes the
// marches about it until its own come round. The mesh drawer builds and binds the
// volumes (SceneMeshDrawer::recordVolume), so a volume's uniforms, buffers
// and textures are a custom shader's.
//
// A volume whose module gives its near medium point by point (medium /
// medium_range, volume_near.bk) has a near grid of its own: before the volumes draw,
// PassVolumeNear integrates the medium over the grid's cells (kNearTile
// pixels a side, kNearSlices deep), and the volume's pixels read the grid
// to their opaque depth in place of marching that part of the ray. A grid
// lives while its volume draws and follows the frame's size.
//
// PassVolumeComposite then blends the result over the HDR scope as scene *
// transmittance + light: after the opaque surfaces, their sky and the
// decals, before SSR and the translucent surfaces.

#include "scene/vulkan/frame/scene_pass.h"
#include "scene/vulkan/frame/scene_vk_target_format.h"

#include <bromath/mat.h>
#include <bromath/vec.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace bro::scene::vk {

class SceneVkAllocator;

class PassVolumeNear final : public ScenePass {
public:
    /// A near grid's cell, pixels a side, and its slices (volume.bk's
    /// NEAR_SLICES).
    static constexpr uint32_t kNearTile = 8;
    static constexpr uint32_t kNearSlices = 128;

    /// A volume's near grid: light (rgb) and transmittance (a), and the
    /// distance weighted by opacity, each to a slice's far end; in GENERAL.
    struct Grid {
        SceneVkImage light, depth;
        uint32_t frame = 0;   // the render that wrote it last
    };

    const char* name() const override { return "volumes near"; }
    bool setup(SceneGpu& gpu) override;
    void resize(SceneGpu& gpu, uint32_t width, uint32_t height) override;
    bool active(const SceneFrame& frame) const override;
    void declare(const SceneFrame& frame, PassIO& io) const override;
    void record(SceneFrame& frame) override;
    void cleanup(SceneGpu& gpu) override;

    /// The volume's grid this render wrote, else null.
    const Grid* grid(const SceneFrame& frame, uint32_t nodeId) const;

private:
    /// The volume's grid at the frame's size, made or remade as needed;
    /// null when it cannot be made.
    Grid* make(SceneFrame& frame, uint32_t nodeId);

    SceneVkDevice* device_ = nullptr;
    SceneVkAllocator* allocator_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    std::unordered_map<uint32_t, Grid> grids_;
};

class PassVolume final : public ScenePass {
public:
    /// A marched pixel's share of its result where last render's is found
    /// (a pixel is marched every fourth render).
    static constexpr float kBlend = 0.1f;

    explicit PassVolume(const PassVolumeNear& near) : near_(near) {}

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
    /// post/postfx.bk's VolumeResolveUBO.
    struct Uniforms {
        float prevViewProj[16];   // last render's camera-relative clip
        float invViewProj[16];    // this render's, inverted
        float eyeDelta[4];        // xyz: this render's eye - last render's; w: 1 when there is no history
        float interleave[4];      // xy: the 2 x 2 block's pixel marched; z: kBlend
    };

    const PassVolumeNear& near_;
    SceneVkDevice* device_ = nullptr;
    SceneVkImage medium_, mediumDepth_;
    SceneVkImage history_[2];
    // Beside each history image, how many marches its pixels' results hold
    // (over 16, to kMarches): a pixel blends its march in at 1 / count, so
    // its first march after it came into view replaces what stood in for
    // it, and at kBlend once it holds kMarches.
    SceneVkImage counts_[2];
    static constexpr VkFormat kCountFormat = VK_FORMAT_R8_UNORM;
    // Every pixel's newest march at the frame's size (post/postfx.bk
    // volume_latest_frag), this render's and last render's, and beside each
    // whether the pixel holds one (1) or what stands in for it (0).
    SceneVkImage latestFrame_[2];
    SceneVkImage real_[2];
    VkPipeline latest_ = VK_NULL_HANDLE;
    // Every image above, for making and releasing them together.
    std::array<SceneVkImage*, 10> images();
    // A full-frame pass of `pipeline` with `set` into `light` and the R8
    // image beside it (both in COLOR_ATTACHMENT_OPTIMAL).
    void drawPair(VkCommandBuffer cmd, const SceneVkImage& light, const SceneVkImage& r8, VkPipeline pipeline,
                  VkDescriptorSet set);
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
