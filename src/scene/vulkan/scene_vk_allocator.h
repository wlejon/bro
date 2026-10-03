#pragma once

#include "scene/vulkan/scene_vk_device.h"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

/// Encapsulates an allocated Vulkan buffer and its backing memory.
struct SceneVkBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags memoryProperties = 0;
    void* mappedData = nullptr;

    bool isValid() const { return buffer != VK_NULL_HANDLE; }
};

/// Encapsulates an allocated Vulkan image, its view, sampler, and state.
struct SceneVkImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    bool isValid() const { return image != VK_NULL_HANDLE; }
};

/// Configuration parameters for creating a 2D scene texture.
struct TextureDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    bool generateMipmaps = true;
    VkFilter minFilter = VK_FILTER_LINEAR;
    VkFilter magFilter = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    VkSamplerAddressMode addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    bool enableAnisotropy = true;
    float maxAnisotropy = 16.0f;
};

/// Allocator and resource manager for 3D scene buffers, images, mipmaps, and transfers.
class SceneVkAllocator {
public:
    explicit SceneVkAllocator(SceneVkDevice& device);
    ~SceneVkAllocator();

    SceneVkAllocator(const SceneVkAllocator&) = delete;
    SceneVkAllocator& operator=(const SceneVkAllocator&) = delete;

    // Buffer management
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memProps, SceneVkBuffer& outBuffer);

    bool createVertexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer);
    bool createIndexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer);
    bool createUniformBuffer(VkDeviceSize size, SceneVkBuffer& outBuffer);

    bool updateUniformBuffer(SceneVkBuffer& buffer, const void* data, VkDeviceSize size, VkDeviceSize offset = 0);
    void destroyBuffer(SceneVkBuffer& buffer);

    // Staging and transfers
    bool stageAndUploadBuffer(VkBuffer dstBuffer, const void* data, VkDeviceSize size, VkDeviceSize dstOffset = 0);
    bool stageAndUploadImage(VkImage dstImage, uint32_t width, uint32_t height,
                             const void* data, VkDeviceSize size, uint32_t mipLevels = 1);

    // Image & Texture management
    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                     SceneVkImage& outImage, uint32_t mipLevels = 1,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                     VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

    bool createTexture2D(const void* pixelData, const TextureDesc& desc, SceneVkImage& outImage);
    void destroyImage(SceneVkImage& image);

    // Image layout transition and mipmap generation
    void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels = 1, uint32_t baseMipLevel = 0,
                               VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

    void generateMipmaps(VkCommandBuffer cmd, VkImage image, VkFormat format,
                         int32_t texWidth, int32_t texHeight, uint32_t mipLevels);

    SceneVkDevice& device() { return device_; }
    const SceneVkDevice& device() const { return device_; }

private:
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    SceneVkDevice& device_;
};

} // namespace bro::scene::vk
