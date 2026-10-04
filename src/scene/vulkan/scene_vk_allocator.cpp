#include "scene/vulkan/scene_vk_allocator.h"
#include "scene/vulkan/scene_vk_memory_pool.h"
#include "util/log.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

SceneVkAllocator::SceneVkAllocator(SceneVkDevice& device)
    : pool_(std::make_unique<SceneVkMemoryPool>()),
      device_(device)
{
}

SceneVkAllocator::~SceneVkAllocator() {
    if (pool_) {
        pool_->cleanup(device_.device());
    }
}

SceneVkAllocatorStats SceneVkAllocator::stats() const {
    if (pool_) {
        return pool_->stats();
    }
    return {};
}

uint32_t SceneVkAllocator::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const {
    const VkPhysicalDeviceMemoryProperties& memProperties = device_.context().memoryProperties();
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    LOG_ERROR("SceneVkAllocator: Failed to find suitable memory type");
    return 0;
}

bool SceneVkAllocator::allocateMemory(VkDeviceSize size, VkDeviceSize alignment, uint32_t memoryTypeIndex,
                                      VkMemoryPropertyFlags properties, bool isImage,
                                      uint64_t& outId, VkDeviceMemory& outMemory,
                                      VkDeviceSize& outOffset, void*& outMappedData) {
    if (!pool_) return false;
    return pool_->allocate(device_.device(), size, alignment, memoryTypeIndex, properties, isImage,
                           outId, outMemory, outOffset, outMappedData);
}

void SceneVkAllocator::freeMemory(uint64_t allocId) {
    if (pool_ && allocId != 0) {
        pool_->free(device_.device(), allocId);
    }
}

bool SceneVkAllocator::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                     VkMemoryPropertyFlags memProps, SceneVkBuffer& outBuffer) {
    if (size == 0) return false;
    VkDevice dev = device_.device();

    VkBufferCreateInfo bufInfo{};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = size;
    bufInfo.usage = usage;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(dev, &bufInfo, nullptr, &outBuffer.buffer) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to create buffer of size %zu", static_cast<size_t>(size));
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(dev, outBuffer.buffer, &memReqs);

    uint32_t memType = findMemoryType(memReqs.memoryTypeBits, memProps);

    uint64_t allocId = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* mappedData = nullptr;

    if (!allocateMemory(memReqs.size, memReqs.alignment, memType, memProps, /*isImage=*/false,
                        allocId, memory, offset, mappedData)) {
        LOG_ERROR("SceneVkAllocator: Failed to allocate %zu bytes for buffer", static_cast<size_t>(memReqs.size));
        vkDestroyBuffer(dev, outBuffer.buffer, nullptr);
        outBuffer.buffer = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindBufferMemory(dev, outBuffer.buffer, memory, offset) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to bind buffer memory");
        vkDestroyBuffer(dev, outBuffer.buffer, nullptr);
        outBuffer.buffer = VK_NULL_HANDLE;
        freeMemory(allocId);
        return false;
    }

    outBuffer.memory = memory;
    outBuffer.size = size;
    outBuffer.offset = offset;
    outBuffer.usage = usage;
    outBuffer.memoryProperties = memProps;
    outBuffer.mappedData = mappedData;
    outBuffer.allocId = allocId;

    return true;
}

bool SceneVkAllocator::createVertexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer) {
    bool ok = createBuffer(size,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           outBuffer);
    if (!ok) return false;

    if (initialData != nullptr) {
        return stageAndUploadBuffer(outBuffer.buffer, initialData, size);
    }
    return true;
}

bool SceneVkAllocator::createIndexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer) {
    bool ok = createBuffer(size,
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                           outBuffer);
    if (!ok) return false;

    if (initialData != nullptr) {
        return stageAndUploadBuffer(outBuffer.buffer, initialData, size);
    }
    return true;
}

bool SceneVkAllocator::createUniformBuffer(VkDeviceSize size, SceneVkBuffer& outBuffer) {
    bool ok = createBuffer(size,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           outBuffer);
    if (!ok) return false;

    if (!outBuffer.mappedData) {
        VkResult res = vkMapMemory(device_.device(), outBuffer.memory, outBuffer.offset, size, 0, &outBuffer.mappedData);
        if (res != VK_SUCCESS) {
            LOG_ERROR("SceneVkAllocator: Failed to map uniform buffer memory");
            destroyBuffer(outBuffer);
            return false;
        }
    }
    return true;
}

bool SceneVkAllocator::updateUniformBuffer(SceneVkBuffer& buffer, const void* data, VkDeviceSize size, VkDeviceSize offset) {
    if (!buffer.isValid() || !data || (offset + size > buffer.size)) {
        return false;
    }

    if (buffer.mappedData) {
        std::memcpy(static_cast<char*>(buffer.mappedData) + offset, data, size);
        return true;
    }

    void* mapped = nullptr;
    if (vkMapMemory(device_.device(), buffer.memory, buffer.offset + offset, size, 0, &mapped) == VK_SUCCESS) {
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_.device(), buffer.memory);
        return true;
    }
    return false;
}

void SceneVkAllocator::destroyBuffer(SceneVkBuffer& buffer) {
    if (!buffer.isValid()) return;
    VkDevice dev = device_.device();

    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(dev, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.allocId != 0) {
        freeMemory(buffer.allocId);
        buffer.allocId = 0;
    } else if (buffer.memory != VK_NULL_HANDLE) {
        if (buffer.mappedData) {
            vkUnmapMemory(dev, buffer.memory);
        }
        vkFreeMemory(dev, buffer.memory, nullptr);
    }
    buffer.memory = VK_NULL_HANDLE;
    buffer.mappedData = nullptr;
    buffer.size = 0;
    buffer.offset = 0;
}

bool SceneVkAllocator::stageAndUploadBuffer(VkBuffer dstBuffer, const void* data, VkDeviceSize size, VkDeviceSize dstOffset) {
    if (!data || size == 0) return true;

    SceneVkBuffer staging;
    bool ok = createBuffer(size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging);
    if (!ok) return false;

    if (staging.mappedData) {
        std::memcpy(staging.mappedData, data, size);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(device_.device(), staging.memory, staging.offset, size, 0, &mapped) != VK_SUCCESS) {
            destroyBuffer(staging);
            return false;
        }
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_.device(), staging.memory);
    }

    device_.executeImmediate([&](VkCommandBuffer cmd) {
        VkBufferCopy copyRegion{};
        copyRegion.srcOffset = 0;
        copyRegion.dstOffset = dstOffset;
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, staging.buffer, dstBuffer, 1, &copyRegion);
    });

    destroyBuffer(staging);
    return true;
}

bool SceneVkAllocator::stageAndUploadImage(VkImage dstImage, uint32_t width, uint32_t height,
                                          const void* data, VkDeviceSize size, uint32_t mipLevels) {
    if (!data || size == 0) return false;

    SceneVkBuffer staging;
    bool ok = createBuffer(size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging);
    if (!ok) return false;

    if (staging.mappedData) {
        std::memcpy(staging.mappedData, data, size);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(device_.device(), staging.memory, staging.offset, size, 0, &mapped) != VK_SUCCESS) {
            destroyBuffer(staging);
            return false;
        }
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_.device(), staging.memory);
    }

    device_.executeImmediate([&](VkCommandBuffer cmd) {
        transitionImageLayout(cmd, dstImage, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              mipLevels, 0);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(cmd, staging.buffer, dstImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    });

    destroyBuffer(staging);
    return true;
}

bool SceneVkAllocator::stageAndUploadImageLayer(VkImage dstImage, VkFormat format,
                                                uint32_t width, uint32_t height,
                                                uint32_t layer, uint32_t mipLevel,
                                                const void* data, VkDeviceSize size) {
    if (!data || size == 0 || width == 0 || height == 0) return false;
    SceneVkBuffer staging;
    bool ok = createBuffer(size,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           staging);
    if (!ok) return false;

    if (staging.mappedData) {
        std::memcpy(staging.mappedData, data, size);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(device_.device(), staging.memory, staging.offset, size, 0, &mapped) != VK_SUCCESS) {
            destroyBuffer(staging);
            return false;
        }
        std::memcpy(mapped, data, size);
        vkUnmapMemory(device_.device(), staging.memory);
    }

    device_.executeImmediate([&](VkCommandBuffer cmd) {
        transitionImageLayout(cmd, dstImage, format,
                              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              1, mipLevel, VK_IMAGE_ASPECT_COLOR_BIT, 1, layer);

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = mipLevel;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(cmd, staging.buffer, dstImage,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        transitionImageLayout(cmd, dstImage, format,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              1, mipLevel, VK_IMAGE_ASPECT_COLOR_BIT, 1, layer);
    });

    destroyBuffer(staging);
    return true;
}

bool SceneVkAllocator::createImage(uint32_t width, uint32_t height, VkFormat format,
                                   VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                                   SceneVkImage& outImage, uint32_t mipLevels,
                                   VkSampleCountFlagBits samples, VkImageAspectFlags aspectMask,
                                   uint32_t arrayLayers, VkImageCreateFlags createFlags,
                                   VkImageViewType viewType) {
    if (width == 0 || height == 0 || arrayLayers == 0) return false;
    VkDevice dev = device_.device();

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.flags = createFlags;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent.width = width;
    imgInfo.extent.height = height;
    imgInfo.extent.depth = 1;
    imgInfo.mipLevels = mipLevels;
    imgInfo.arrayLayers = arrayLayers;
    imgInfo.format = format;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imgInfo.usage = usage;
    imgInfo.samples = samples;
    imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(dev, &imgInfo, nullptr, &outImage.image) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to create VkImage (%ux%u, format %d)", width, height, format);
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(dev, outImage.image, &memReqs);

    uint32_t memType = findMemoryType(memReqs.memoryTypeBits, memProps);

    uint64_t allocId = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* mappedData = nullptr;

    if (!allocateMemory(memReqs.size, memReqs.alignment, memType, memProps, /*isImage=*/true,
                        allocId, memory, offset, mappedData)) {
        LOG_ERROR("SceneVkAllocator: Failed to allocate memory for VkImage");
        vkDestroyImage(dev, outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(dev, outImage.image, memory, offset) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to bind VkImage memory");
        vkDestroyImage(dev, outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        freeMemory(allocId);
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage.image;
    if (viewType != VK_IMAGE_VIEW_TYPE_MAX_ENUM) {
        viewInfo.viewType = viewType;
    } else {
        viewInfo.viewType = (createFlags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
            ? VK_IMAGE_VIEW_TYPE_CUBE
            : (arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
    }
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = arrayLayers;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &outImage.view) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to create VkImageView");
        destroyImage(outImage);
        return false;
    }

    outImage.memory = memory;
    outImage.offset = offset;
    outImage.allocId = allocId;
    outImage.format = format;
    outImage.width = width;
    outImage.height = height;
    outImage.mipLevels = mipLevels;
    outImage.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outImage.sampler = VK_NULL_HANDLE;

    return true;
}

bool SceneVkAllocator::createTexture2D(const void* pixelData, const TextureDesc& desc, SceneVkImage& outImage) {
    if (desc.width == 0 || desc.height == 0) return false;

    uint32_t mipLevels = desc.generateMipmaps
        ? static_cast<uint32_t>(std::floor(std::log2(std::max(desc.width, desc.height)))) + 1
        : 1;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.generateMipmaps && mipLevels > 1) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }

    bool ok = createImage(desc.width, desc.height, desc.format,
                          usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          outImage, mipLevels, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!ok) return false;

    if (pixelData) {
        uint32_t bpp = 4;
        if (desc.format == VK_FORMAT_R8_UNORM) bpp = 1;
        else if (desc.format == VK_FORMAT_R32_SFLOAT) bpp = 4;
        else if (desc.format == VK_FORMAT_R16G16B16A16_SFLOAT) bpp = 8;
        else if (desc.format == VK_FORMAT_R32G32B32A32_SFLOAT) bpp = 16;
        VkDeviceSize imageSize = static_cast<VkDeviceSize>(desc.width) * desc.height * bpp;
        if (!stageAndUploadImage(outImage.image, desc.width, desc.height, pixelData, imageSize, mipLevels)) {
            destroyImage(outImage);
            return false;
        }

        if (desc.generateMipmaps && mipLevels > 1) {
            device_.executeImmediate([&](VkCommandBuffer cmd) {
                generateMipmaps(cmd, outImage.image, desc.format, desc.width, desc.height, mipLevels);
            });
            outImage.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            device_.executeImmediate([&](VkCommandBuffer cmd) {
                transitionImageLayout(cmd, outImage.image, desc.format,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      mipLevels, 0);
            });
            outImage.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    // Create Sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = desc.magFilter;
    samplerInfo.minFilter = desc.minFilter;
    samplerInfo.addressModeU = desc.addressModeU;
    samplerInfo.addressModeV = desc.addressModeV;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = desc.enableAnisotropy ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = desc.maxAnisotropy;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = desc.mipmapMode;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = static_cast<float>(mipLevels);

    if (vkCreateSampler(device_.device(), &samplerInfo, nullptr, &outImage.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to create sampler for texture");
        destroyImage(outImage);
        return false;
    }

    return true;
}

bool SceneVkAllocator::createTexture3D(const void* voxelData, uint32_t size, VkFormat format, SceneVkImage& outImage) {
    if (size == 0) return false;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent.width = size;
    imageInfo.extent.height = size;
    imageInfo.extent.depth = size;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(device_.device(), &imageInfo, nullptr, &outImage.image) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed creating 3D image");
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device_.device(), outImage.image, &memReqs);

    uint32_t memType = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uint64_t allocId = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* mappedData = nullptr;

    if (!allocateMemory(memReqs.size, memReqs.alignment, memType,
                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true,
                        allocId, memory, offset, mappedData)) {
        LOG_ERROR("SceneVkAllocator: Failed allocating memory for 3D image");
        vkDestroyImage(device_.device(), outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(device_.device(), outImage.image, memory, offset) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to bind 3D image memory");
        vkDestroyImage(device_.device(), outImage.image, nullptr);
        outImage.image = VK_NULL_HANDLE;
        freeMemory(allocId);
        return false;
    }

    outImage.memory = memory;
    outImage.offset = offset;
    outImage.allocId = allocId;
    outImage.width = size;
    outImage.height = size;
    outImage.mipLevels = 1;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device_.device(), &viewInfo, nullptr, &outImage.view) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed creating image view for 3D image");
        destroyImage(outImage);
        return false;
    }

    outImage.format = format;
    outImage.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (voxelData) {
        VkDeviceSize dataSize = static_cast<VkDeviceSize>(size) * size * size * 4;
        SceneVkBuffer staging;
        if (!createBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          staging)) {
            destroyImage(outImage);
            return false;
        }

        if (staging.mappedData) {
            std::memcpy(staging.mappedData, voxelData, dataSize);
        } else {
            void* mapped = nullptr;
            if (vkMapMemory(device_.device(), staging.memory, staging.offset, dataSize, 0, &mapped) == VK_SUCCESS) {
                std::memcpy(mapped, voxelData, dataSize);
                vkUnmapMemory(device_.device(), staging.memory);
            }
        }

        device_.executeImmediate([&](VkCommandBuffer cmd) {
            transitionImageLayout(cmd, outImage.image, format,
                                  VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  1, 0, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0);

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {size, size, size};

            vkCmdCopyBufferToImage(cmd, staging.buffer, outImage.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &region);

            transitionImageLayout(cmd, outImage.image, format,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  1, 0, VK_IMAGE_ASPECT_COLOR_BIT, 1, 0);
        });

        destroyBuffer(staging);
        outImage.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(device_.device(), &samplerInfo, nullptr, &outImage.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed creating sampler for 3D image");
        destroyImage(outImage);
        return false;
    }

    return true;
}

void SceneVkAllocator::generateMipmaps(VkCommandBuffer cmd, VkImage image, VkFormat format,
                                      int32_t texWidth, int32_t texHeight, uint32_t mipLevels,
                                      uint32_t baseArrayLayer, uint32_t layerCount) {
    // Check linear blitting support
    VkFormatProperties formatProperties;
    vkGetPhysicalDeviceFormatProperties(device_.physicalDevice(), format, &formatProperties);
    if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        LOG_WARN("SceneVkAllocator: Texture image format does not support linear blitting!");
        return;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevels; ++i) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = baseArrayLayer;
        blit.srcSubresource.layerCount = layerCount;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = baseArrayLayer;
        blit.dstSubresource.layerCount = layerCount;

        vkCmdBlitImage(cmd,
                       image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit,
                       VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr,
                             0, nullptr,
                             1, &barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

void SceneVkAllocator::transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat format,
                                            VkImageLayout oldLayout, VkImageLayout newLayout,
                                            uint32_t mipLevels, uint32_t baseMipLevel,
                                            VkImageAspectFlags aspectMask,
                                            uint32_t layerCount, uint32_t baseArrayLayer) {
    (void)format;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = baseMipLevel;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = baseArrayLayer;
    barrier.subresourceRange.layerCount = layerCount;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        destinationStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd,
                         sourceStage, destinationStage,
                         0,
                         0, nullptr,
                         0, nullptr,
                         1, &barrier);
}

void SceneVkAllocator::destroyImage(SceneVkImage& image) {
    if (!image.isValid()) return;
    VkDevice dev = device_.device();

    if (image.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(dev, image.sampler, nullptr);
        image.sampler = VK_NULL_HANDLE;
    }
    if (image.view != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, image.view, nullptr);
        image.view = VK_NULL_HANDLE;
    }
    if (image.image != VK_NULL_HANDLE) {
        vkDestroyImage(dev, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
    }
    if (image.allocId != 0) {
        freeMemory(image.allocId);
        image.allocId = 0;
    } else if (image.memory != VK_NULL_HANDLE) {
        vkFreeMemory(dev, image.memory, nullptr);
    }
    image.memory = VK_NULL_HANDLE;
    image.offset = 0;
    image.width = 0;
    image.height = 0;
    image.mipLevels = 1;
    image.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

} // namespace bro::scene::vk
