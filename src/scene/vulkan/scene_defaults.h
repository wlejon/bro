#pragma once

// What every scene pass shares and nothing owns: the descriptor-set layouts
// of the lit pipelines (so a set written for one pass binds in any other), the
// 1x1 fallback images a set is filled with where a node has no texture, and
// the default sampler.

#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_descriptors.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class SceneDefaults {
public:
    bool setup(SceneVkDevice& device, SceneVkAllocator& allocator);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    // --- Set layouts of the lit pipelines ---
    // set 0: camera UBO; set 1: lighting UBO + shadow atlas, then the probe
    // cube, shade map and IBL as bare images under one shared sampler; set 2: material (albedo, normal, metallic-roughness,
    // emissive, occlusion); set 3: per-draw vertex data, the bone palette UBO
    // (binding 0) or a procedural draw's segment records (binding 1, storage);
    // set 4: custom shader UBO + 8 samplers.
    VkDescriptorSetLayout cameraLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout boneLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout customLayout = VK_NULL_HANDLE;
    /// Bound where a pipeline layout has a set the pass never uses.
    VkDescriptorSetLayout emptyLayout = VK_NULL_HANDLE;

    // --- Fallback images (1x1) ---
    SceneVkImage white;     // (1,1,1,1)
    SceneVkImage flatNormal;// (0.5,0.5,1,1): tangent-space +Z
    SceneVkImage black;     // (0,0,0,1)
    SceneVkImage cube;      // black cube map
    VkSampler sampler = VK_NULL_HANDLE;      // linear, clamp to edge
    VkSampler cubeSampler = VK_NULL_HANDLE;  // linear mips, clamp to edge

    /// Material set of all fallbacks (written once, never changes).
    VkDescriptorSet defaultMaterialSet = VK_NULL_HANDLE;

private:
    SceneVkDescriptorPool pool_;
};

}  // namespace bro::scene::vk
