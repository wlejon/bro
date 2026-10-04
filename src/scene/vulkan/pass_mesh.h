#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_pipeline.h"
#include "scene/vulkan/scene_vk_descriptors.h"
#include "scene/vulkan/scene_vk_target.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// Uniform push constant block uploaded per draw call. Total: 112 bytes (guaranteed <= 128 bytes).
struct alignas(16) MeshPushConstants {
    float model[16];        // 64 bytes: model matrix
    float baseColor[4];     // 16 bytes: RGBA base color tint
    float emissive[4];      // 16 bytes: rgb tint, a = intensity
    float pbrParams[4];     // 16 bytes: x=metallic, y=roughness, z=alphaCutoff, w=flags (bit0: albedo, bit1: normal, bit2: mr, bit3: emissive)
};

/// Parameters for issuing a single static mesh draw call.
struct MeshDrawCall {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    uint32_t indexCount = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;

    float modelMatrix[16];
    float baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissiveColor[3] = {0.0f, 0.0f, 0.0f};
    float emissiveIntensity = 0.0f;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float alphaCutoff = 0.0f;
    uint32_t flags = 0;

    VkDescriptorSet materialSet = VK_NULL_HANDLE; // Set 2 (if null, default dummy textures are used)
};

/// Parameters for issuing an instanced mesh draw call.
struct InstancedMeshDrawCall : public MeshDrawCall {
    VkBuffer instanceBuffer = VK_NULL_HANDLE;
    VkDeviceSize instanceOffset = 0;
    uint32_t instanceCount = 0;
};

/// Parameters for issuing a GPU-skinned mesh draw call.
struct SkinnedMeshDrawCall : public MeshDrawCall {
    VkBuffer skinAttribBuffer = VK_NULL_HANDLE;
    VkDeviceSize skinAttribOffset = 0;
    VkDescriptorSet bonePaletteSet = VK_NULL_HANDLE; // Set 3: 256 mat4 bone matrices UBO
};

/// Forward PBR mesh rendering pass supporting static, instanced, and skinned geometry.
class PassMesh {
public:
    struct Config {
        Config() = default;
        VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
        VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
        bool depthWrite = true;
        VkCompareOp depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // Reversed-Z
    };

    PassMesh() = default;
    ~PassMesh();

    PassMesh(const PassMesh&) = delete;
    PassMesh& operator=(const PassMesh&) = delete;

    /// Initialize descriptor layouts, default textures, pipeline layout, and graphics pipelines.
    bool init(SceneVkDevice& device, SceneVkAllocator& allocator);
    bool init(SceneVkDevice& device, SceneVkAllocator& allocator, const Config& config);

    /// Clean up pipelines, layouts, and dummy textures.
    void cleanup(SceneVkDevice& device, SceneVkAllocator& allocator);

    /// Begin mesh rendering pass recording in command buffer.
    void begin(VkCommandBuffer cmd,
               VkDescriptorSet cameraSet,
               VkDescriptorSet lightingSet,
               uint32_t viewportWidth,
               uint32_t viewportHeight);

    /// Draw a static mesh.
    void drawStatic(VkCommandBuffer cmd, const MeshDrawCall& draw);
    void drawStaticTranslucent(VkCommandBuffer cmd, const MeshDrawCall& draw);

    /// Draw an instanced mesh batch.
    void drawInstanced(VkCommandBuffer cmd, const InstancedMeshDrawCall& draw);
    void drawInstancedTranslucent(VkCommandBuffer cmd, const InstancedMeshDrawCall& draw);

    /// Draw a skinned mesh.
    void drawSkinned(VkCommandBuffer cmd, const SkinnedMeshDrawCall& draw);
    void drawSkinnedTranslucent(VkCommandBuffer cmd, const SkinnedMeshDrawCall& draw);

    // Layout accessors
    VkPipelineLayout pipelineLayout() const { return pipelineLayout_; }
    VkDescriptorSetLayout cameraLayout() const { return cameraLayout_; }
    VkDescriptorSetLayout lightingLayout() const { return lightingLayout_; }
    VkDescriptorSetLayout materialLayout() const { return materialLayout_; }
    VkDescriptorSetLayout bonePaletteLayout() const { return bonePaletteLayout_; }
    VkDescriptorSet defaultMaterialSet() const { return defaultMaterialSet_; }
    VkImageView dummyWhiteView() const { return dummyWhiteImage_.view; }
    VkImageView dummyNormalView() const { return dummyNormalImage_.view; }
    VkImageView dummyBlackView() const { return dummyBlackImage_.view; }
    VkSampler defaultSampler() const { return defaultSampler_; }

private:
    bool createDescriptorLayouts(VkDevice device);
    bool createPipelines(VkDevice device, const Config& config);
    bool createDefaultTextures(SceneVkDevice& device, SceneVkAllocator& allocator);

    VkDescriptorSetLayout cameraLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightingLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bonePaletteLayout_ = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;

    VkPipeline pipelineStatic_ = VK_NULL_HANDLE;
    VkPipeline pipelineInstanced_ = VK_NULL_HANDLE;
    VkPipeline pipelineSkinned_ = VK_NULL_HANDLE;

    VkPipeline pipelineStaticTranslucent_ = VK_NULL_HANDLE;
    VkPipeline pipelineInstancedTranslucent_ = VK_NULL_HANDLE;
    VkPipeline pipelineSkinnedTranslucent_ = VK_NULL_HANDLE;

    // Default dummy material resources (1x1 white, 1x1 flat normal, 1x1 white MR, 1x1 black emissive)
    SceneVkImage dummyWhiteImage_;
    SceneVkImage dummyNormalImage_;
    SceneVkImage dummyBlackImage_;
    VkSampler defaultSampler_ = VK_NULL_HANDLE;
    SceneVkDescriptorPool defaultDescPool_;
    VkDescriptorSet defaultMaterialSet_ = VK_NULL_HANDLE;

    // Active state during begin() ... draws
    VkDescriptorSet activeCameraSet_ = VK_NULL_HANDLE;
    VkDescriptorSet activeLightingSet_ = VK_NULL_HANDLE;
    uint32_t viewportWidth_ = 0;
    uint32_t viewportHeight_ = 0;
};

} // namespace bro::scene::vk
