#pragma once

// The cloud deck on the GPU (scene/world/clouds.h; docs/clouds-api.js;
// docs/scene-world-architecture.md §13 "volumes"): the world's module, its
// cover, the engine's noise, the march and the shadow.
//
//   - The module's set (CloudShaders, a ModuleShaders set): the march
//     (sky/clouds.frag), the cover's bake (sky/clouds_cover.comp) and the
//     shadow's levels (sky/clouds_shadow.comp), each with the world's
//     module in sky/cloud_module.glsl's place and, for a deck set over a
//     land with processes, its grid's layout in ground/land_grid_config.glsl's.
//   - The noise (sky/clouds_noise.comp): one tile of tiling noise and its
//     mips, made once when a scene first has clouds.
//   - The cover: the module's cloudCover over a cube about the body, baked
//     when the clouds are set (and when the land they read is ready).
//   - The shadow: what the deck leaves of the sun over CL_SHADOW_LEVELS
//     levels about the body's point under the eye, remade every frame the
//     deck is drawn, read by every lit shader (the lighting set's binding
//     24; scene_lighting.glsl's sceneCloudSun).
//   - The march: drawn over the opaque surfaces in the HDR scope
//     (pass_clouds.h).
//
// Nothing of it runs, and nothing is allocated, in a scene without clouds:
// its passes are off, the lighting block says no shadow and binding 24 is a
// texel the shaders never read.

#include "scene/vulkan/frame/scene_module_shaders.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::scene::vk {

struct SceneFrame;
struct SceneGpu;

/// The shaders a deck is drawn with.
class CloudShaders : public ModuleShaders {
public:
    enum Stage : int {
        March,    // clouds.frag
        Cover,    // clouds_cover.comp
        Shadow,   // clouds_shadow.comp
        kStages
    };
    static constexpr BuiltinSceneShader kBuiltin[kStages] = {
        BuiltinSceneShader::CloudsFrag, BuiltinSceneShader::CloudsCoverComp, BuiltinSceneShader::CloudsShadowComp};

    /// The set with `module` as cloud_module.glsl and `grid` (or none, "")
    /// as land_grid_config.glsl.
    bool create(VkDevice device, const std::string& module, const std::string& grid, std::string& err) {
        Overrides overrides{{"sky/cloud_module.glsl", module}};
        if (!grid.empty()) overrides.push_back({"ground/land_grid_config.glsl", grid});
        return ModuleShaders::create(device, kBuiltin, overrides, err);
    }

    VkShaderModule operator[](Stage s) const { return module(s); }
    VkPipeline cover = VK_NULL_HANDLE;
    VkPipeline shadow = VK_NULL_HANDLE;
};

class SceneClouds {
public:
    bool setup(SceneGpu& gpu);
    void cleanup(SceneGpu& gpu);

    /// The frame begins (after the atmosphere's, before its lighting is
    /// filled): whether the deck is drawn, its module compiled, whether its
    /// cover is to be baked, and where the shadow's levels lie.
    void beginFrame(SceneFrame& frame);
    /// The deck is drawn this frame.
    bool active() const { return on_; }
    /// The lighting block's shadow (off: params[0] 0) and the lighting set's
    /// binding 24.
    void fill(CloudShadowGpu& out) const { out = shadowBlock_; }
    VkImageView shadowView() const;

    /// The noise (once), the cover (when it is to be baked) and the
    /// shadow's levels, into the graphics stream outside any rendering,
    /// before anything lit reads them (PassCloudsPrepare).
    void recordPrepare(SceneFrame& frame);
    /// The march, in the HDR scope over the opaque surfaces (PassClouds).
    void recordDraw(SceneFrame& frame);

    /// The clouds' block (std140) — CloudsUBO in sky/scene_cloud_layer.glsl.
    struct alignas(16) Block {
        float toBody[3][4];
        float shell[4];
        float params[4];
        float sun[4];
        float shadowN[4];
        float shadowE1[4];
        float shadowE2[4];
        float data[64][4];
    };

private:
    CloudShaders* shaders(SceneFrame& frame);
    bool ensureImages(SceneGpu& gpu);
    bool createNoise(SceneGpu& gpu);
    void recordNoise(SceneFrame& frame);
    VkDescriptorSet writeSet(SceneFrame& frame, bool march, bool bake);

    SceneVkDevice* device_ = nullptr;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout marchLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout computeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout noiseSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout noiseLayout_ = VK_NULL_HANDLE;
    VkShaderModule skyVs_ = VK_NULL_HANDLE;
    VkShaderModule noiseCs_ = VK_NULL_HANDLE;
    VkPipeline noisePipeline_ = VK_NULL_HANDLE;
    VkSampler coverSampler_ = VK_NULL_HANDLE;
    VkSampler noiseSampler_ = VK_NULL_HANDLE;

    // The module sets, by their sources' hash (-1: did not compile).
    std::vector<std::unique_ptr<CloudShaders>> sets_;
    std::unordered_map<uint64_t, int> byHash_;
    CloudShaders* current_ = nullptr;
    uint64_t currentKey_ = 0;    // its module's and grid layout's hash

    // The noise: a 3D image, its mips, a storage view a mip and a sampled view of all.
    struct Noise {
        VkImage image = VK_NULL_HANDLE;
        uint64_t allocId = 0;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView sampled = VK_NULL_HANDLE;
        VkImageView mips[16] = {};
        bool made = false;
    } noise_;
    SceneVkImage cover_, shadow_;
    SceneVkImage coverNone_, shadowNone_, gridNone_;

    Block block_{};
    CloudShadowGpu shadowBlock_{};
    bool on_ = false;
    bool shadowsOn_ = false;
    bool bake_ = false;          // the cover is baked this frame
    VkImageView bakeGrid_ = VK_NULL_HANDLE;
    uint64_t bakedGeneration_ = 0;
    uint64_t bakedHash_ = 0;
};

}  // namespace bro::scene::vk
