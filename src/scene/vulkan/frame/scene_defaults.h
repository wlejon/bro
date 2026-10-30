#pragma once

// What every scene pass shares and nothing owns: the descriptor-set layouts
// of the lit pipelines (so a set written for one pass binds in any other), the
// 1x1 fallback images a set is filled with where a node has no texture, and
// the default sampler.

#include "scene/nodes/mesh_node.h"
#include "scene/vulkan/frame/scene_vk_allocator.h"
#include "scene/vulkan/frame/scene_vk_descriptors.h"

#include <vulkan/vulkan.h>

namespace bro::scene::vk {

class SceneDefaults {
public:
    bool setup(SceneVkDevice& device, SceneVkAllocator& allocator);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    // --- Set layouts of the lit pipelines ---
    // set 0: camera UBO; set 1: lighting UBO + shadow pages, then the probe
    // cube, shade map and IBL as bare images under one shared sampler; set 2: material (albedo, normal, metallic-roughness,
    // emissive, occlusion); set 3: per-draw vertex data, the bone palette UBO
    // (binding 0) or a procedural draw's segment records (binding 1, storage);
    // set 4: custom shader UBO + kCustomImages images (bindings 1 to
    // kCustomImages), then the samplers they are read through
    // (CustomShaderInterface::kSamplersBinding, immutable). A set written for
    // this layout fills every image binding.
    static constexpr uint32_t kCustomImages = MeshNode::kMaxUserTextures;
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
    SceneVkImage zeroArray; // one-layer 2D array of zeros (an unset sampler2DArray)
    SceneVkImage zeroVolume;      // a 1x1x1 3D image of zeros (an unset sampler3D)
    SceneVkImage zeroUintArray;   // the same, R32_UINT (an unset usampler2DArray: an empty page table)
    VkSampler sampler = VK_NULL_HANDLE;      // linear, clamp to edge
    VkSampler cubeSampler = VK_NULL_HANDLE;  // linear mips, clamp to edge
    VkSampler nearestSampler = VK_NULL_HANDLE;   // nearest, clamp to edge (integer images)
    VkSampler repeatSampler = VK_NULL_HANDLE;    // linear, repeating (wrapped clipmaps)
    VkSampler mipSampler = VK_NULL_HANDLE;       // linear, every mip, clamp to edge
    VkSampler repeatSSampler = VK_NULL_HANDLE;   // linear, every mip, repeating in s, clamped in t

    /// Material set of all fallbacks (written once, never changes).
    VkDescriptorSet defaultMaterialSet = VK_NULL_HANDLE;

private:
    SceneVkDescriptorPool pool_;
};

}  // namespace bro::scene::vk
