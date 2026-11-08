#include "webgl/vulkan/webgl_vk_canvas.h"
#include "webgl/vulkan/webgl_vk_formats.h"
#include "webgl/vulkan/webgl_vk_stream.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>

namespace bro::webgl::vk {

WebGLVkCanvas::WebGLVkCanvas(render::VulkanContext& context, WebGLVkStream& stream,
                             const ContextAttributes& attributes)
    : context_(context), stream_(stream), attributes_(attributes)
{
}

WebGLVkCanvas::~WebGLVkCanvas() {
    release(0);
}

bool WebGLVkCanvas::init(uint32_t width, uint32_t height, uint64_t afterTicket) {
    release(afterTicket);

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

bool WebGLVkCanvas::resize(uint32_t width, uint32_t height, uint64_t afterTicket) {
    uint32_t w = std::max(1u, width);
    uint32_t h = std::max(1u, height);

    if (w == width_ && h == height_ && isValid()) {
        return true;
    }

    return init(w, h, afterTicket);
}

void WebGLVkCanvas::release(uint64_t afterTicket) {
    VkDevice dev = context_.device();
    if (dev == VK_NULL_HANDLE) return;

    render::VulkanContext* ctx = &context_;
    struct Dead { VkImage image; VkImageView view; VkImageView extraView; uint64_t allocId; };
    Dead color{colorImage_, colorView_, VK_NULL_HANDLE, colorAllocId_};
    Dead front{front_.image, front_.view, front_.presentView, front_.allocId};
    Dead depth{depthImage_, depthView_, VK_NULL_HANDLE, depthAllocId_};
    if (color.image != VK_NULL_HANDLE || front.image != VK_NULL_HANDLE || depth.image != VK_NULL_HANDLE) {
        stream_.deferUntil(afterTicket, [ctx, dev, color, front, depth] {
            for (const Dead& d : {color, front, depth}) {
                if (d.view != VK_NULL_HANDLE) vkDestroyImageView(dev, d.view, nullptr);
                if (d.extraView != VK_NULL_HANDLE) vkDestroyImageView(dev, d.extraView, nullptr);
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
    front_ = ColorImage{};
    frontLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
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
    // Fresh images: nothing to keep, and nothing used them before.
    colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    clearImages(cmd);
}

void WebGLVkCanvas::recordClear(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE) return;
    if (needsInit_) {
        recordInit(cmd);
        return;
    }
    clearImages(cmd);
}

// The images' transitions run from their current layouts, so they wait for
// what read or wrote them last (the compositor's sampling included).
void WebGLVkCanvas::clearImages(VkCommandBuffer cmd) {
    if (colorImage_ != VK_NULL_HANDLE) {
        const VkImageSubresourceRange range = render::colorRange();
        transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clear{};
        if (opaque()) clear.float32[3] = 1.0f;
        vkCmdClearColorImage(cmd, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (depthImage_ != VK_NULL_HANDLE) {
        const VkImageSubresourceRange range{render::imageAspectFor(depthFormat_), 0, 1, 0, 1};
        transitionDepth(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, depthImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        transitionDepth(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }
}

bool WebGLVkCanvas::createColorImage(uint32_t width, uint32_t height, VkImageUsageFlags usage, ColorImage& out) {
    VkDevice dev = context_.device();
    if (!context_.createImage(width, height, colorFormat_, VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, out.image, out.memory, out.offset, out.allocId)) {
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = out.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = colorFormat_;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(dev, &viewInfo, nullptr, &out.view) != VK_SUCCESS) return false;
    if (opaque() && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
        if (vkCreateImageView(dev, &viewInfo, nullptr, &out.presentView) != VK_SUCCESS) return false;
    }
    return true;
}

bool WebGLVkCanvas::createColorAttachment(uint32_t width, uint32_t height) {
    colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;

    ColorImage back;
    const bool made = createColorImage(width, height,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                       back);
    colorImage_ = back.image;
    colorMemory_ = back.memory;
    colorOffset_ = back.offset;
    colorAllocId_ = back.allocId;
    colorView_ = back.view;
    if (!made) return false;
    // The front: what the presenter samples, and copies from (a blend).
    if (!createColorImage(width, height,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          front_)) {
        return false;
    }
    frontLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    needsInit_ = true;
    return true;
}

bool WebGLVkCanvas::createDepthAttachment(uint32_t width, uint32_t height) {
    VkDevice dev = context_.device();
    // GL's default framebuffer is DEPTH24_STENCIL8 with both, DEPTH_COMPONENT24
    // or STENCIL_INDEX8 with one: the same Vulkan format as a renderbuffer of
    // that format, so depth blits between them work.
    depthFormat_ = VK_FORMAT_UNDEFINED;
    if (!attributes_.depth && !attributes_.stencil) return true;
    const GLenum internalformat = !attributes_.stencil ? GL_DEPTH_COMPONENT24
                                  : !attributes_.depth ? GL_STENCIL_INDEX8
                                                       : GL_DEPTH24_STENCIL8;
    depthFormat_ = depthStencilFormat(context_.physicalDevice(), internalformat);

    VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!context_.createImage(width, height, depthFormat_,
                              VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              depthImage_, depthMemory_,
                              depthOffset_, depthAllocId_)) {
        return false;
    }

    const VkImageAspectFlags aspectMask = render::imageAspectFor(depthFormat_);

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

void WebGLVkCanvas::recordPresentCopy(VkCommandBuffer cmd) {
    if (!isValid() || front_.image == VK_NULL_HANDLE || cmd == VK_NULL_HANDLE) return;
    recordInit(cmd);
    transitionColor(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    // After every earlier read of the front (the presenter's sampling, or its
    // copy in a blend, in submissions before this one): nothing kept.
    render::ImageBarrier toDst;
    toDst.image = front_.image;
    toDst.oldLayout = frontLayout_;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    toDst.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toDst.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    render::cmdImageBarrier(cmd, toDst);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width_, height_, 1};
    vkCmdCopyImage(cmd, colorImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, front_.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Readable by whatever samples or copies it in later submissions.
    render::ImageBarrier toRead;
    toRead.image = front_.image;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toRead.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    toRead.dstAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    render::cmdImageBarrier(cmd, toRead);
    frontLayout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

render::LayerImage WebGLVkCanvas::frontImage() const {
    render::LayerImage img;
    if (front_.image == VK_NULL_HANDLE) return img;
    img.image = front_.image;
    img.view = front_.presentView != VK_NULL_HANDLE ? front_.presentView : front_.view;
    img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img.format = colorFormat_;
    img.width = width_;
    img.height = height_;
    return img;
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
