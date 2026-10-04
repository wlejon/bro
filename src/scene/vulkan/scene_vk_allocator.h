#pragma once

#include "scene/vulkan/scene_vk_device.h"
#include "render/vulkan_memory_pool.h"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

using SceneVkAllocatorStats = render::VulkanAllocatorStats;

/// Encapsulates an allocated Vulkan buffer and its backing memory.
struct SceneVkBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize offset = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags memoryProperties = 0;
    void* mappedData = nullptr;
    uint64_t allocId = 0;

    bool isValid() const { return buffer != VK_NULL_HANDLE; }
};

/// Encapsulates an allocated Vulkan image, its view, sampler, and state.
/// `currentLayout` is the layout of every subresource as of the end of the
/// commands recorded so far (uploads included).
struct SceneVkImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDeviceSize offset = 0;
    uint64_t allocId = 0;

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

/// A rectangle of one or more layers of mip 0, for uploads. A zero width or
/// height means the whole level.
struct ImageRegion {
    uint32_t layer = 0;
    uint32_t layerCount = 1;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// Buffers, images and uploads for the 3D scene, on the context's memory pool.
///
/// Uploads go through the device's upload stream (staging in the frame's
/// upload memory, copies in the upload command buffer) and never wait.
/// Destruction is deferred until the GPU is done with the resource, so a
/// resource can be replaced while frames that use it are in flight.
class SceneVkAllocator {
public:
    explicit SceneVkAllocator(SceneVkDevice& device);
    ~SceneVkAllocator() = default;

    SceneVkAllocator(const SceneVkAllocator&) = delete;
    SceneVkAllocator& operator=(const SceneVkAllocator&) = delete;

    // Buffer management
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags memProps, SceneVkBuffer& outBuffer);
    /// Device-local vertex / index buffers, filled through the upload stream.
    bool createVertexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer);
    bool createIndexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer);
    void destroyBuffer(SceneVkBuffer& buffer);

    /// Record a copy of `data` into `dstBuffer` at `dstOffset`.
    bool stageAndUploadBuffer(VkBuffer dstBuffer, const void* data, VkDeviceSize size, VkDeviceSize dstOffset = 0);

    // Image & Texture management
    /// Create an image and its view. A sampled image starts out (once the
    /// upload stream runs) in SHADER_READ_ONLY_OPTIMAL, cleared to zero, so it
    /// is valid to bind before anything renders into it.
    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                     SceneVkImage& outImage, uint32_t mipLevels = 1,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
                     VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                     uint32_t arrayLayers = 1,
                     VkImageCreateFlags createFlags = 0,
                     VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM);

    bool createTexture2D(const void* pixelData, const TextureDesc& desc, SceneVkImage& outImage);
    bool createTexture3D(const void* voxelData, uint32_t size, VkFormat format, SceneVkImage& outImage);

    /// Record an upload of `size` bytes of tightly packed texels into
    /// `region` of mip 0 of a sampled image; with `regenerateMips` the rest of
    /// the mip chain of those layers is rebuilt from it. The image is
    /// sampleable afterwards.
    bool uploadImage(SceneVkImage& image, const void* data, VkDeviceSize size,
                     const ImageRegion& region = {}, bool regenerateMips = false);

    void destroyImage(SceneVkImage& image);

    /// Layout transition with masks derived from the layouts (render::cmdTransitionImage).
    void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels = 1, uint32_t baseMipLevel = 0,
                               VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               uint32_t layerCount = 1, uint32_t baseArrayLayer = 0);

    /// Build mips 1..mipLevels-1 of the given layers from mip 0. Every level
    /// must be in TRANSFER_DST_OPTIMAL (mip 0 holding the source); all end in
    /// SHADER_READ_ONLY_OPTIMAL.
    void generateMipmaps(VkCommandBuffer cmd, VkImage image, VkFormat format,
                         int32_t texWidth, int32_t texHeight, uint32_t mipLevels,
                         uint32_t baseArrayLayer = 0, uint32_t layerCount = 1);

    SceneVkAllocatorStats stats() const;

    SceneVkDevice& device() { return device_; }
    const SceneVkDevice& device() const { return device_; }

private:
    SceneVkDevice& device_;
};

} // namespace bro::scene::vk
