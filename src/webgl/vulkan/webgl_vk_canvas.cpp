#include "webgl/vulkan/webgl_vk_canvas.h"
#include "render/vulkan_util.h"
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

    render::VulkanContext* ctx = &context_;
    struct Dead { VkImage image; VkImageView view; uint64_t allocId; };
    Dead color{colorImage_, colorView_, colorAllocId_};
    Dead depth{depthImage_, depthView_, depthAllocId_};
    if (color.image != VK_NULL_HANDLE || depth.image != VK_NULL_HANDLE) {
        context_.frames().defer([ctx, dev, color, depth] {
            for (const Dead& d : {color, depth}) {
                if (d.view != VK_NULL_HANDLE) vkDestroyImageView(dev, d.view, nullptr);
                if (d.image != VK_NULL_HANDLE) ctx->destroyImage(d.image, d.allocId);
            }
        });
    }
    colorImage_ = VK_NULL_HANDLE;
    colorView_ = VK_NULL_HANDLE;
    colorMemory_ = VK_NULL_HANDLE;
    colorAllocId_ = 0;
    colorOffset_ = 0;
    colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImage_ = VK_NULL_HANDLE;
    depthView_ = VK_NULL_HANDLE;
    depthMemory_ = VK_NULL_HANDLE;
    depthAllocId_ = 0;
    depthOffset_ = 0;
    depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    needsInit_ = false;
}

void WebGLVkCanvas::recordInit(VkCommandBuffer cmd) {
    if (!needsInit_ || cmd == VK_NULL_HANDLE) return;
    needsInit_ = false;
    if (colorImage_ != VK_NULL_HANDLE) {
        const VkImageSubresourceRange range = render::colorRange();
        render::cmdTransitionImage(cmd, colorImage_, range, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        colorLayout_ = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        const VkImageSubresourceRange range{render::imageAspectFor(depthFormat_), 0, 1, 0, 1};
        render::cmdTransitionImage(cmd, depthImage_, range, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, depthImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        depthLayout_ = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
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
    colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    needsInit_ = true;
    return true;
}

bool WebGLVkCanvas::createDepthAttachment(uint32_t width, uint32_t height) {
    VkDevice dev = context_.device();
    depthFormat_ = findSupportedDepthFormat();

    VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

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
    depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    needsInit_ = true;
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

bool WebGLVkCanvas::recordCopy(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset) {
    if (!isValid() || cmd == VK_NULL_HANDLE || dst == VK_NULL_HANDLE) return false;
    recordInit(cmd);
    const VkImageLayout restore = colorLayout_;
    transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = dstOffset;
    copyRegion.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copyRegion.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(cmd, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &copyRegion);
    transitionColor(cmd, restore);
    return true;
}

} // namespace bro::webgl::vk
