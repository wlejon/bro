#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::scene::vk {

/// Camera and viewport uniform block structure matching GLSL std140 layout.
struct alignas(16) SceneCameraUniforms {
    alignas(16) float view[16];
    alignas(16) float proj[16];
    alignas(16) float viewProj[16];
    alignas(16) float invView[16];
    alignas(16) float invProj[16];
    alignas(16) float eyePos[4];     // xyz = position, w = time
    alignas(16) float viewport[4];   // x = width, y = height, z = near, w = far
    alignas(16) float fogParams[4];  // x = start, y = end, z = density, w = mode
    alignas(16) float fogColor[4];   // rgb = color, a = heightFalloff
};

/// Global directional/ambient lighting uniform block structure matching GLSL std140 layout.
struct alignas(16) SceneLightingUniforms {
    alignas(16) float sunDirection[4];       // xyz = normalized light vector, w = enabled
    alignas(16) float sunColor[4];           // rgb = sun color, a = intensity
    alignas(16) float ambientColor[4];       // rgb = ambient color, a = sky irradiance factor
    alignas(16) float shadowSplits[4];       // x,y,z,w = split distances for 4 cascades
    alignas(16) float shadowCascadeProj[16]; // cascade 0 projection (or atlas transform)
    alignas(16) float numLights[4];          // x = point light count, y = spot count, z = shadow enabled
};

/// Utility for constructing VkDescriptorSetLayouts with arbitrary bindings.
class SceneVkDescriptorLayoutBuilder {
public:
    void addBinding(uint32_t binding, VkDescriptorType type, uint32_t count, VkShaderStageFlags stageFlags);
    void clear();

    VkDescriptorSetLayout build(VkDevice device);

    // Standard preset layout builders for the 3D scene engine
    static VkDescriptorSetLayout createCameraLayout(VkDevice device);
    static VkDescriptorSetLayout createLightingLayout(VkDevice device);
    static VkDescriptorSetLayout createMaterialLayout(VkDevice device, uint32_t samplerCount = 4);

private:
    std::vector<VkDescriptorSetLayoutBinding> bindings_;
};

/// Helper for recording and dispatching VkWriteDescriptorSet updates.
class SceneVkDescriptorWriter {
public:
    void writeBuffer(uint32_t binding, VkBuffer buffer, VkDeviceSize size,
                     VkDeviceSize offset = 0,
                     VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    void writeImage(uint32_t binding, VkImageView imageView, VkSampler sampler,
                    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VkDescriptorType type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    void clear();
    void updateSet(VkDevice device, VkDescriptorSet set);

private:
    std::vector<VkDescriptorBufferInfo> bufferInfos_;
    std::vector<VkDescriptorImageInfo> imageInfos_;
    std::vector<VkWriteDescriptorSet> writes_;
};

/// Manages descriptor set allocation from a Vulkan descriptor pool.
class SceneVkDescriptorPool {
public:
    SceneVkDescriptorPool() = default;
    ~SceneVkDescriptorPool();

    bool init(VkDevice device, uint32_t maxSets = 256,
              const std::vector<VkDescriptorPoolSize>& customSizes = {});
    void destroy();

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void reset();

    VkDescriptorPool handle() const { return pool_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
};

/// Per-frame dynamic descriptor allocator and cache to allow allocating
/// descriptors on the fly each frame and resetting them efficiently.
class SceneVkDescriptorCache {
public:
    explicit SceneVkDescriptorCache(VkDevice device);
    ~SceneVkDescriptorCache();

    bool init(uint32_t maxSetsPerFrame = 256);
    void destroy();

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);
    void reset();

private:
    VkDevice device_ = VK_NULL_HANDLE;
    SceneVkDescriptorPool pool_;
};

} // namespace bro::scene::vk
