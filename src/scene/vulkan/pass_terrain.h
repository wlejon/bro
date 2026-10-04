#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>

#include <unordered_map>

namespace bro::scene {
class SceneGraph;
class MeshNode;
}

namespace bro::scene::vk {

/// Pass for rendering clipmap terrain LOD rings with height displacement,
/// normal maps, procedural detail, and tile shade mapping.
class PassTerrain {
public:
    PassTerrain() = default;
    ~PassTerrain();

    PassTerrain(const PassTerrain&) = delete;
    PassTerrain& operator=(const PassTerrain&) = delete;

    bool init(SceneVkDevice& device, SceneVkAllocator& allocator,
              VkDescriptorSetLayout cameraLayout, VkDescriptorSetLayout lightingLayout);
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    void render(VkCommandBuffer cmd, MeshNode* terrainNode,
                VkDescriptorSet cameraSet, VkDescriptorSet lightingSet,
                uint32_t viewportWidth, uint32_t viewportHeight,
                VkBuffer vertexBuffer, VkBuffer indexBuffer, uint32_t indexCount);

    void setSampleCount(VkSampleCountFlagBits samples) {
        if (sampleCount_ != samples) {
            sampleCount_ = samples;
            for (int h = 0; h < 2; ++h) {
                for (int s = 0; s < 2; ++s) {
                    if (pipelines_[h][s] != VK_NULL_HANDLE && device_) {
                        vkDestroyPipeline(device_->device(), pipelines_[h][s], nullptr);
                        pipelines_[h][s] = VK_NULL_HANDLE;
                    }
                }
            }
        }
    }

private:
    struct NodeTerrainResources {
        SceneVkImage heightsImage;
        SceneVkImage surfacesImage;
        SceneVkBuffer terrainUbo;
        VkDescriptorSet terrainDescSet = VK_NULL_HANDLE;
        int currentHeightsW = 0, currentHeightsH = 0, currentHeightsLayers = 0;
        int currentSurfsW = 0, currentSurfsH = 0, currentSurfsLayers = 0;
    };

    VkPipeline getOrCreatePipeline(bool cubicHeight, bool cubicSurface);
    NodeTerrainResources& getOrCreateNodeResources(MeshNode* node);
    bool syncTextures(MeshNode* node, NodeTerrainResources& res);
    void syncUniforms(MeshNode* node, NodeTerrainResources& res);

    SceneVkDevice* device_ = nullptr;
    SceneVkAllocator* allocator_ = nullptr;
    VkDescriptorSetLayout cameraLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout terrainLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    VkSampleCountFlagBits sampleCount_ = VK_SAMPLE_COUNT_1_BIT;
    // Pipelines indexed by [cubicHeight(0/1)][cubicSurface(0/1)]
    VkPipeline pipelines_[2][2] = {{VK_NULL_HANDLE, VK_NULL_HANDLE}, {VK_NULL_HANDLE, VK_NULL_HANDLE}};

    VkSampler heightsSampler_ = VK_NULL_HANDLE;
    VkSampler surfacesSampler_ = VK_NULL_HANDLE;

    SceneVkDescriptorPool terrainDescPool_;
    std::unordered_map<const void*, NodeTerrainResources> nodeResources_;
};

} // namespace bro::scene::vk

