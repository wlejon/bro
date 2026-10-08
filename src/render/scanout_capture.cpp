#include "render/scanout_capture.h"

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

namespace bro::render {

ScanoutCapture::ScanoutCapture(VulkanContext& ctx, uint32_t width, uint32_t height, Sink sink)
    : ctx_(ctx), width_(width), height_(height), sink_(std::move(sink)) {
    if (!ctx_.createImage(width_, height_, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image_, imageMemory_, imageOffset_, imageAlloc_)) {
        LOG_WARN("ScanoutCapture: creating the %ux%u capture image failed", width_, height_);
        image_ = VK_NULL_HANDLE;
        return;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(width_) * height_ * 4;
    // Cached host memory reads back at memcpy speed; plain coherent memory
    // (write-combined on most devices) is far slower to read.
    if (!ctx_.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                           buffer_, bufferMemory_, bufferOffset_, bufferAlloc_, mapped_) &&
        !ctx_.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer_,
                           bufferMemory_, bufferOffset_, bufferAlloc_, mapped_)) {
        LOG_WARN("ScanoutCapture: creating the %llu-byte readback buffer failed",
                 static_cast<unsigned long long>(size));
        buffer_ = VK_NULL_HANDLE;
        mapped_ = nullptr;
    }
}

ScanoutCapture::~ScanoutCapture() {
    if (buffer_ != VK_NULL_HANDLE) ctx_.destroyBuffer(buffer_, bufferAlloc_);
    if (image_ != VK_NULL_HANDLE) ctx_.destroyImage(image_, imageAlloc_);
}

void ScanoutCapture::record(VkCommandBuffer cmd, VkImage src, uint32_t srcW, uint32_t srcH) {
    pending_ = false;
    if (!valid()) return;

    // The composite's writes, visible to the blit that reads them.
    ImageBarrier toRead;
    toRead.image = src;
    toRead.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toRead.srcStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    toRead.srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toRead.dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    cmdImageBarrier(cmd, toRead);

    cmdTransitionImage(cmd, image_, colorRange(),
                       imageInitialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    imageInitialized_ = true;

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(srcW), static_cast<int32_t>(srcH), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(width_), static_cast<int32_t>(height_), 1};
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    cmdTransitionImage(cmd, image_, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width_, height_, 1};
    vkCmdCopyImageToBuffer(cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer_, 1, &copy);
    cmdBufferBarrier(cmd, buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);

    // The blit read the scanout image in GENERAL; leave it there for KMS.
    pending_ = true;
}

void ScanoutCapture::completed(const KmsDirectPresenter::FlipInfo& flip) {
    if (!pending_ || !sink_) return;
    pending_ = false;
    sink_(static_cast<const uint8_t*>(mapped_), width_, height_, flip);
}

}  // namespace bro::render
