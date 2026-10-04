#include "webgl/vulkan/webgl_vk_canvas.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

WebGLVkCanvas::WebGLVkCanvas(render::VulkanContext& context)
    : context_(context)
{
}

WebGLVkCanvas::~WebGLVkCanvas() {
    cleanup();
}

VkFormat WebGLVkCanvas::findSupportedDepthFormat() {
    VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM
    };

    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(context_.physicalDevice(), format, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return format;
        }
    }
    return VK_FORMAT_D32_SFLOAT;
}

bool WebGLVkCanvas::init(uint32_t width, uint32_t height) {
    cleanup();

    width_ = std::max(1u, width);
    height_ = std::max(1u, height);

    if (!createColorAttachment(width_, height_)) {
        LOG_ERROR("WebGLVkCanvas: Failed to create color attachment");
        return false;
    }

    if (!createDepthAttachment(width_, height_)) {
        LOG_ERROR("WebGLVkCanvas: Failed to create depth attachment");
        return false;
    }

    return true;
}

bool WebGLVkCanvas::resize(uint32_t width, uint32_t height) {
    uint32_t w = std::max(1u, width);
    uint32_t h = std::max(1u, height);

    if (w == width_ && h == height_ && isValid()) {
        return true;
    }

    return init(w, h);
}

void WebGLVkCanvas::cleanup() {
    VkDevice dev = context_.device();
    if (dev == VK_NULL_HANDLE) return;

    context_.waitIdle();

    if (colorView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, colorView_, nullptr);
        colorView_ = VK_NULL_HANDLE;
    }
    if (colorAllocId_ != 0) {
        context_.destroyImage(colorImage_, colorAllocId_);
    } else {
        if (colorImage_ != VK_NULL_HANDLE) vkDestroyImage(dev, colorImage_, nullptr);
        if (colorMemory_ != VK_NULL_HANDLE) vkFreeMemory(dev, colorMemory_, nullptr);
    }
    colorImage_ = VK_NULL_HANDLE;
    colorMemory_ = VK_NULL_HANDLE;
    colorAllocId_ = 0;
    colorOffset_ = 0;
    colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    if (depthView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, depthView_, nullptr);
        depthView_ = VK_NULL_HANDLE;
    }
    if (depthAllocId_ != 0) {
        context_.destroyImage(depthImage_, depthAllocId_);
    } else {
        if (depthImage_ != VK_NULL_HANDLE) vkDestroyImage(dev, depthImage_, nullptr);
        if (depthMemory_ != VK_NULL_HANDLE) vkFreeMemory(dev, depthMemory_, nullptr);
    }
    depthImage_ = VK_NULL_HANDLE;
    depthMemory_ = VK_NULL_HANDLE;
    depthAllocId_ = 0;
    depthOffset_ = 0;
    depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    if (readbackAllocId_ != 0) {
        context_.destroyBuffer(readbackBuffer_, readbackAllocId_);
    } else {
        if (readbackBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(dev, readbackBuffer_, nullptr);
        if (readbackMemory_ != VK_NULL_HANDLE) vkFreeMemory(dev, readbackMemory_, nullptr);
    }
    readbackBuffer_ = VK_NULL_HANDLE;
    readbackMemory_ = VK_NULL_HANDLE;
    readbackAllocId_ = 0;
    readbackOffset_ = 0;
    readbackMapped_ = nullptr;
    readbackBufferSize_ = 0;
}

bool WebGLVkCanvas::createColorAttachment(uint32_t width, uint32_t height) {
    VkDevice dev = context_.device();
    colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!context_.createImage(width, height, colorFormat_,
                              VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              colorImage_, colorMemory_,
                              colorOffset_, colorAllocId_)) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = colorImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = colorFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &colorView_) != VK_SUCCESS) {
        return false;
    }

    // Transition initial layout to COLOR_ATTACHMENT_OPTIMAL
    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    context_.transitionImageLayout(colorImage_, colorFormat_,
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   cmd);
    context_.endSingleTimeCommands(cmd);
    colorLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    return true;
}

bool WebGLVkCanvas::createDepthAttachment(uint32_t width, uint32_t height) {
    VkDevice dev = context_.device();
    depthFormat_ = findSupportedDepthFormat();

    VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!context_.createImage(width, height, depthFormat_,
                              VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              depthImage_, depthMemory_,
                              depthOffset_, depthAllocId_)) {
        return false;
    }

    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (depthFormat_ == VK_FORMAT_D32_SFLOAT_S8_UINT || depthFormat_ == VK_FORMAT_D24_UNORM_S8_UINT) {
        aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depthImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depthFormat_;
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &depthView_) != VK_SUCCESS) {
        return false;
    }

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    context_.transitionImageLayout(depthImage_, depthFormat_,
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                   cmd);
    context_.endSingleTimeCommands(cmd);
    depthLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    return true;
}

void WebGLVkCanvas::transitionColor(VkCommandBuffer cmd, VkImageLayout newLayout) {
    if (colorLayout_ == newLayout || colorImage_ == VK_NULL_HANDLE) return;
    context_.transitionImageLayout(colorImage_, colorFormat_, colorLayout_, newLayout, cmd);
    colorLayout_ = newLayout;
}

void WebGLVkCanvas::transitionDepth(VkCommandBuffer cmd, VkImageLayout newLayout) {
    if (depthLayout_ == newLayout || depthImage_ == VK_NULL_HANDLE) return;
    context_.transitionImageLayout(depthImage_, depthFormat_, depthLayout_, newLayout, cmd);
    depthLayout_ = newLayout;
}

bool WebGLVkCanvas::readCanvasPixels(std::vector<uint8_t>& out) {
    if (!isValid() || width_ == 0 || height_ == 0) return false;

    VkDevice dev = context_.device();
    VkDeviceSize requiredSize = static_cast<VkDeviceSize>(width_) * height_ * 4;

    if (readbackBuffer_ == VK_NULL_HANDLE || readbackBufferSize_ < requiredSize) {
        if (readbackAllocId_ != 0) {
            context_.destroyBuffer(readbackBuffer_, readbackAllocId_);
        } else {
            if (readbackBuffer_ != VK_NULL_HANDLE) vkDestroyBuffer(dev, readbackBuffer_, nullptr);
            if (readbackMemory_ != VK_NULL_HANDLE) vkFreeMemory(dev, readbackMemory_, nullptr);
        }
        readbackBuffer_ = VK_NULL_HANDLE;
        readbackMemory_ = VK_NULL_HANDLE;
        readbackAllocId_ = 0;
        readbackOffset_ = 0;
        readbackMapped_ = nullptr;

        readbackBufferSize_ = requiredSize * 2;
        if (!context_.createBuffer(readbackBufferSize_,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   readbackBuffer_, readbackMemory_,
                                   readbackOffset_, readbackAllocId_, readbackMapped_)) {
            LOG_ERROR("WebGLVkCanvas: Failed to allocate readback staging buffer");
            return false;
        }
    }

    VkImageLayout oldLayout = colorLayout_;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent = {width_, height_, 1};

    vkCmdCopyImageToBuffer(cmd, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readbackBuffer_, 1, &copyRegion);

    transitionColor(cmd, oldLayout);
    context_.endSingleTimeCommands(cmd);

    context_.waitIdle();

    if (readbackMapped_) {
        out.resize(requiredSize);
        std::memcpy(out.data(), readbackMapped_, requiredSize);
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(dev, readbackMemory_, readbackOffset_, requiredSize, 0, &mapped) != VK_SUCCESS) {
            LOG_ERROR("WebGLVkCanvas: Failed to map readback buffer memory");
            return false;
        }
        out.resize(requiredSize);
        std::memcpy(out.data(), mapped, requiredSize);
        vkUnmapMemory(dev, readbackMemory_);
    }

    return true;
}

} // namespace bro::webgl::vk
