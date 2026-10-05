#pragma once

// The scene's environment: the image-based lighting baked from the
// renderer's equirect HDR, and the sky drawn behind everything.
//
// IBL. When SceneRenderer::environmentGeneration() moves, update() takes the
// decoded panorama and bakes it, in the frame's command buffer ahead of the
// passes: the radiance cube (1024², mipmapped — the sky and the source of
// the rest), its cosine-convolved irradiance (32²) and its GGX prefilter
// (256², 6 mips, roughness 0..1 down the chain). The split-sum BRDF LUT is
// environment-independent and baked once, on first use. The lighting set
// binds all four (fallbacks where absent) and the lighting block says
// whether they are live (fillLighting). Reflection probes prefilter their
// captures with the same GGX pass (prefilter()).
//
// Sky. drawSky() draws, into whatever target is open (the HDR scope, a probe
// face), the atmosphere when it is enabled, else the radiance cube when an
// environment is loaded, then the starfield added on top — the GL renderer's
// order. Perspective cameras only: an orthographic view has no direction.

#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target_format.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace bro::scene {
class SceneRenderer;
}

namespace bro::scene::vk {

struct SceneGpu;
class SceneDefaults;

class SceneEnvironment {
public:
    bool setup(SceneVkDevice& device, SceneVkAllocator& allocator, const SceneDefaults& defaults);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    /// Bake a newly loaded environment, or drop a cleared one. Records into
    /// `cmd` outside any rendering.
    void update(SceneGpu& gpu, VkCommandBuffer cmd, SceneRenderer& renderer);
    /// Bake the BRDF LUT if it has not been yet (outside any rendering).
    bool ensureBrdfLut(SceneGpu& gpu, VkCommandBuffer cmd);

    /// The lighting block's image-based lighting and atmosphere fields.
    void fillLighting(SceneLightingUniforms& out, const SceneRenderer& renderer) const;

    /// The lighting set's environment bindings (4 irradiance, 5 prefilter,
    /// 6 BRDF LUT, 7 radiance cube), fallbacks where not baked.
    void writeBindings(SceneVkDescriptorWriter& writer, const SceneDefaults& defaults) const;

    /// Whether drawSky() draws anything for `renderer` through a camera.
    bool skyVisible(const SceneRenderer& renderer, bool perspective) const;
    /// Draw the sky into the open rendering of format `target`.
    void drawSky(SceneGpu& gpu, VkCommandBuffer cmd, const TargetFormat& target, VkDescriptorSet cameraSet,
                 VkDescriptorSet lightingSet, const SceneRenderer& renderer);

    /// GGX-prefilter `src` (a mipmapped cube in SHADER_READ_ONLY_OPTIMAL)
    /// into every mip of `dst` (a cube, colour-attachment usable), mip k at
    /// roughness k / (mips - 1). `dst` ends in SHADER_READ_ONLY_OPTIMAL.
    bool prefilter(SceneGpu& gpu, VkCommandBuffer cmd, const SceneVkImage& src, SceneVkImage& dst);

    static constexpr VkFormat kCubeFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

private:
    struct BakePush {
        int32_t face;
        float roughness;
        float envSize;
        float pad;
    };

    bool bake(SceneGpu& gpu, VkCommandBuffer cmd, SceneRenderer& renderer);
    void release(SceneVkAllocator& allocator);
    /// A 2D view of one face and mip of `image`, destroyed once the frame is done.
    VkImageView faceView(SceneGpu& gpu, const SceneVkImage& image, uint32_t face, uint32_t mip);
    /// Render `pipeline` into all six faces of mip `mip` of `dst` (in
    /// COLOR_ATTACHMENT_OPTIMAL), sampling `set`.
    void renderFaces(SceneGpu& gpu, VkCommandBuffer cmd, SceneVkImage& dst, uint32_t mip, VkPipeline pipeline,
                     VkDescriptorSet set, BakePush push);
    VkDescriptorSet samplerSet(SceneGpu& gpu, VkImageView view, VkSampler sampler);

    VkDevice dev_ = VK_NULL_HANDLE;

    // Bake: one sampler set, the face / roughness push block.
    VkDescriptorSetLayout bakeSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout bakeLayout_ = VK_NULL_HANDLE;
    VkShaderModule postVs_ = VK_NULL_HANDLE;
    VkShaderModule convertFs_ = VK_NULL_HANDLE;
    VkShaderModule irradianceFs_ = VK_NULL_HANDLE;
    VkShaderModule prefilterFs_ = VK_NULL_HANDLE;
    VkShaderModule brdfFs_ = VK_NULL_HANDLE;
    VkPipeline convertPipeline_ = VK_NULL_HANDLE;
    VkPipeline irradiancePipeline_ = VK_NULL_HANDLE;
    VkPipeline prefilterPipeline_ = VK_NULL_HANDLE;
    VkPipeline brdfPipeline_ = VK_NULL_HANDLE;
    VkSampler equirectSampler_ = VK_NULL_HANDLE;

    // Sky: the camera and lighting sets, the starfield's push block.
    VkPipelineLayout skyLayout_ = VK_NULL_HANDLE;
    VkShaderModule skyVs_ = VK_NULL_HANDLE;
    VkShaderModule atmosphereFs_ = VK_NULL_HANDLE;
    VkShaderModule skyboxFs_ = VK_NULL_HANDLE;
    VkShaderModule starfieldFs_ = VK_NULL_HANDLE;
    PipelineVariants skyPipelines_;

    uint64_t generation_ = 0;
    SceneVkImage radiance_;     // 1024², mipmapped
    SceneVkImage irradiance_;   // 32²
    SceneVkImage prefiltered_;  // 256², 6 mips
    SceneVkImage brdfLut_;      // 512², RG16F
};

}  // namespace bro::scene::vk
