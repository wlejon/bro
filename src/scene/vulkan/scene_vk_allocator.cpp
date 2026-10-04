#include "scene/vulkan/scene_vk_allocator.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::scene::vk {

namespace {

uint32_t bytesPerTexel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
    }
}

} // namespace

SceneVkAllocator::SceneVkAllocator(SceneVkDevice& device)
    : device_(device)
{
}

SceneVkAllocatorStats SceneVkAllocator::stats() const {
    return device_.context().memoryPool().stats();
}

bool SceneVkAllocator::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                     VkMemoryPropertyFlags memProps, SceneVkBuffer& outBuffer) {
    if (size == 0) return false;
    void* mapped = nullptr;
    if (!device_.context().createBuffer(size, usage, memProps, outBuffer.buffer, outBuffer.memory,
                                        outBuffer.offset, outBuffer.allocId, mapped)) {
        LOG_ERROR("SceneVkAllocator: Failed to create a %zu-byte buffer", static_cast<size_t>(size));
        outBuffer = SceneVkBuffer{};
        return false;
    }
    outBuffer.size = size;
    outBuffer.usage = usage;
    outBuffer.memoryProperties = memProps;
    outBuffer.mappedData = mapped;
    return true;
}

bool SceneVkAllocator::createVertexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer) {
    if (!createBuffer(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer))
        return false;
    return !initialData || stageAndUploadBuffer(outBuffer.buffer, initialData, size);
}

bool SceneVkAllocator::createIndexBuffer(VkDeviceSize size, const void* initialData, SceneVkBuffer& outBuffer) {
    if (!createBuffer(size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, outBuffer))
        return false;
    return !initialData || stageAndUploadBuffer(outBuffer.buffer, initialData, size);
}

void SceneVkAllocator::destroyBuffer(SceneVkBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE || buffer.allocId != 0) {
        render::VulkanContext* ctx = &device_.context();
        VkBuffer handle = buffer.buffer;
        uint64_t allocId = buffer.allocId;
        device_.defer([ctx, handle, allocId] { ctx->destroyBuffer(handle, allocId); });
    }
    buffer = SceneVkBuffer{};
}

bool SceneVkAllocator::stageAndUploadBuffer(VkBuffer dstBuffer, const void* data, VkDeviceSize size,
                                            VkDeviceSize dstOffset) {
    if (!data || size == 0) return true;
    render::UploadSlice staging = device_.frameUpload(size, 4);
    if (!staging) return false;
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));
    VkBufferCopy region{staging.offset, dstOffset, size};
    vkCmdCopyBuffer(device_.uploadCommands(), staging.buffer, dstBuffer, 1, &region);
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
    const bool sampled = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
    // Sampled images are cleared once at creation.
    if (sampled) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.flags = createFlags;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.extent = {width, height, 1};
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
        outImage.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(dev, outImage.image, &memReqs);
    const uint32_t memType = device_.context().findMemoryType(memReqs.memoryTypeBits, memProps);
    void* mapped = nullptr;
    if (memType == render::VulkanContext::kNoMemoryType ||
        !device_.context().memoryPool().allocate(dev, memReqs.size, memReqs.alignment, memType, memProps,
                                                 /*isImage=*/true, outImage.allocId, outImage.memory,
                                                 outImage.offset, mapped) ||
        vkBindImageMemory(dev, outImage.image, outImage.memory, outImage.offset) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to allocate or bind memory for a %ux%u image", width, height);
        vkDestroyImage(dev, outImage.image, nullptr);
        if (outImage.allocId != 0) device_.context().memoryPool().free(dev, outImage.allocId);
        outImage = SceneVkImage{};
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
    viewInfo.subresourceRange = {aspectMask, 0, mipLevels, 0, arrayLayers};

    outImage.format = format;
    outImage.width = width;
    outImage.height = height;
    outImage.depth = 1;
    outImage.mipLevels = mipLevels;
    outImage.arrayLayers = arrayLayers;
    outImage.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    outImage.sampler = VK_NULL_HANDLE;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &outImage.view) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed to create VkImageView");
        outImage.view = VK_NULL_HANDLE;
        destroyImage(outImage);
        return false;
    }

    if (sampled && samples == VK_SAMPLE_COUNT_1_BIT) {
        VkCommandBuffer cmd = device_.uploadCommands();
        const VkImageSubresourceRange range{render::imageAspectFor(format), 0, mipLevels, 0, arrayLayers};
        render::cmdTransitionImage(cmd, outImage.image, range, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue zero{};
            vkCmdClearColorImage(cmd, outImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        } else {
            VkClearDepthStencilValue zero{0.0f, 0};
            vkCmdClearDepthStencilImage(cmd, outImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        render::cmdTransitionImage(cmd, outImage.image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        outImage.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    return true;
}

bool SceneVkAllocator::uploadImage(SceneVkImage& image, const void* data, VkDeviceSize size,
                                   const ImageRegion& region, bool regenerateMips) {
    if (!image.isValid() || !data || size == 0 || region.layerCount == 0) return false;
    const uint32_t w = region.width ? region.width : image.width;
    const uint32_t h = region.height ? region.height : image.height;
    render::UploadSlice staging = device_.frameUpload(size, std::max<VkDeviceSize>(bytesPerTexel(image.format), 4));
    if (!staging) return false;
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    VkCommandBuffer cmd = device_.uploadCommands();
    const VkImageAspectFlags aspect = render::imageAspectFor(image.format);
    const VkImageSubresourceRange whole{aspect, 0, image.mipLevels, 0, image.arrayLayers};
    // Every subresource shares one tracked layout; settle the whole image in
    // SHADER_READ_ONLY first if it is anywhere else.
    if (image.currentLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        render::cmdTransitionImage(cmd, image.image, whole, image.currentLayout,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        image.currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    const bool mips = regenerateMips && image.mipLevels > 1;
    const VkImageSubresourceRange dst{aspect, 0, mips ? image.mipLevels : 1u, region.layer, region.layerCount};
    render::cmdTransitionImage(cmd, image.image, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy{};
    copy.bufferOffset = staging.offset;
    copy.imageSubresource = {aspect, 0, region.layer, region.layerCount};
    copy.imageOffset = {region.x, region.y, 0};
    copy.imageExtent = {w, h, image.depth};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    if (mips) {
        generateMipmaps(cmd, image.image, image.format, static_cast<int32_t>(image.width),
                        static_cast<int32_t>(image.height), image.mipLevels, region.layer, region.layerCount);
    } else {
        render::cmdTransitionImage(cmd, image.image, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return true;
}

bool SceneVkAllocator::createTexture2D(const void* pixelData, const TextureDesc& desc, SceneVkImage& outImage) {
    if (desc.width == 0 || desc.height == 0) return false;

    const uint32_t mipLevels = desc.generateMipmaps
        ? static_cast<uint32_t>(std::floor(std::log2(std::max(desc.width, desc.height)))) + 1
        : 1;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (mipLevels > 1) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (!createImage(desc.width, desc.height, desc.format, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     outImage, mipLevels, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;

    if (pixelData) {
        const VkDeviceSize imageSize = static_cast<VkDeviceSize>(desc.width) * desc.height * bytesPerTexel(desc.format);
        if (!uploadImage(outImage, pixelData, imageSize, {}, mipLevels > 1)) {
            destroyImage(outImage);
            return false;
        }
    }

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
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = desc.mipmapMode;
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
    VkDevice dev = device_.device();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_3D;
    imageInfo.extent = {size, size, size};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    if (vkCreateImage(dev, &imageInfo, nullptr, &outImage.image) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed creating 3D image");
        outImage.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(dev, outImage.image, &memReqs);
    const uint32_t memType = device_.context().findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    void* mapped = nullptr;
    if (memType == render::VulkanContext::kNoMemoryType ||
        !device_.context().memoryPool().allocate(dev, memReqs.size, memReqs.alignment, memType,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, outImage.allocId,
                                                 outImage.memory, outImage.offset, mapped) ||
        vkBindImageMemory(dev, outImage.image, outImage.memory, outImage.offset) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed allocating memory for 3D image");
        vkDestroyImage(dev, outImage.image, nullptr);
        if (outImage.allocId != 0) device_.context().memoryPool().free(dev, outImage.allocId);
        outImage = SceneVkImage{};
        return false;
    }
    outImage.width = size;
    outImage.height = size;
    outImage.depth = size;
    outImage.mipLevels = 1;
    outImage.arrayLayers = 1;
    outImage.format = format;
    outImage.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D;
    viewInfo.format = format;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(dev, &viewInfo, nullptr, &outImage.view) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed creating image view for 3D image");
        outImage.view = VK_NULL_HANDLE;
        destroyImage(outImage);
        return false;
    }

    if (voxelData) {
        const VkDeviceSize dataSize = static_cast<VkDeviceSize>(size) * size * size * bytesPerTexel(format);
        if (!uploadImage(outImage, voxelData, dataSize)) {
            destroyImage(outImage);
            return false;
        }
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(dev, &samplerInfo, nullptr, &outImage.sampler) != VK_SUCCESS) {
        LOG_ERROR("SceneVkAllocator: Failed creating sampler for 3D image");
        destroyImage(outImage);
        return false;
    }
    return true;
}

void SceneVkAllocator::generateMipmaps(VkCommandBuffer cmd, VkImage image, VkFormat format,
                                      int32_t texWidth, int32_t texHeight, uint32_t mipLevels,
                                      uint32_t baseArrayLayer, uint32_t layerCount) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device_.physicalDevice(), format, &props);
    const VkFilter filter = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                                ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    auto level = [&](uint32_t mip) {
        return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, baseArrayLayer, layerCount};
    };

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;
    for (uint32_t i = 1; i < mipLevels; ++i) {
        render::cmdTransitionImage(cmd, image, level(i - 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, baseArrayLayer, layerCount};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        mipWidth = std::max(1, mipWidth / 2);
        mipHeight = std::max(1, mipHeight / 2);
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, baseArrayLayer, layerCount};
        blit.dstOffsets[1] = {mipWidth, mipHeight, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        render::cmdTransitionImage(cmd, image, level(i - 1), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    render::cmdTransitionImage(cmd, image, level(mipLevels - 1), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void SceneVkAllocator::transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkFormat /*format*/,
                                            VkImageLayout oldLayout, VkImageLayout newLayout,
                                            uint32_t mipLevels, uint32_t baseMipLevel,
                                            VkImageAspectFlags aspectMask,
                                            uint32_t layerCount, uint32_t baseArrayLayer) {
    render::cmdTransitionImage(cmd, image, {aspectMask, baseMipLevel, mipLevels, baseArrayLayer, layerCount},
                               oldLayout, newLayout);
}

void SceneVkAllocator::destroyImage(SceneVkImage& image) {
    if (image.image != VK_NULL_HANDLE || image.view != VK_NULL_HANDLE || image.sampler != VK_NULL_HANDLE ||
        image.allocId != 0) {
        render::VulkanContext* ctx = &device_.context();
        VkDevice dev = device_.device();
        SceneVkImage dead = image;
        device_.defer([ctx, dev, dead] {
            if (dead.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, dead.sampler, nullptr);
            if (dead.view != VK_NULL_HANDLE) vkDestroyImageView(dev, dead.view, nullptr);
            if (dead.image != VK_NULL_HANDLE) vkDestroyImage(dev, dead.image, nullptr);
            if (dead.allocId != 0) ctx->memoryPool().free(dev, dead.allocId);
        });
    }
    image = SceneVkImage{};
}

} // namespace bro::scene::vk
