#include "render/vulkan_presenter.h"
#include "util/log.h"

#include <include/core/SkColorType.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSurface.h>

#include <cstring>

namespace bro::render {

VulkanPresenter::VulkanPresenter(VulkanContext& context, VulkanSwapchain& swapchain)
    : context_(context), swapchain_(&swapchain)
{
}

VulkanPresenter::VulkanPresenter(VulkanContext& context)
    : context_(context), swapchain_(nullptr)
{
}

VulkanPresenter::~VulkanPresenter() {
    cleanup();
}

bool VulkanPresenter::init() {
    if (swapchain_) {
        // Allocate command buffers matching swapchain frames in flight
        commandBuffers_.resize(VulkanSwapchain::kMaxFramesInFlight);
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = context_.commandPool();
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());

        if (vkAllocateCommandBuffers(context_.device(), &allocInfo, commandBuffers_.data()) != VK_SUCCESS) {
            LOG_ERROR("VulkanPresenter: Failed to allocate command buffers");
            return false;
        }

        width_ = swapchain_->extent().width;
        height_ = swapchain_->extent().height;
    }

    return true;
}

void VulkanPresenter::cleanup() {
    VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device);

    if (!commandBuffers_.empty() && context_.commandPool() != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, context_.commandPool(),
                             static_cast<uint32_t>(commandBuffers_.size()),
                             commandBuffers_.data());
        commandBuffers_.clear();
    }

    if (stagingBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, stagingBuffer_, nullptr);
        stagingBuffer_ = VK_NULL_HANDLE;
    }
    if (stagingMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, stagingMemory_, nullptr);
        stagingMemory_ = VK_NULL_HANDLE;
    }
    stagingBufferSize_ = 0;

    if (readbackBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, readbackBuffer_, nullptr);
        readbackBuffer_ = VK_NULL_HANDLE;
    }
    if (readbackMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, readbackMemory_, nullptr);
        readbackMemory_ = VK_NULL_HANDLE;
    }
    readbackBufferSize_ = 0;

    if (offscreenView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, offscreenView_, nullptr);
        offscreenView_ = VK_NULL_HANDLE;
    }
    if (offscreenImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, offscreenImage_, nullptr);
        offscreenImage_ = VK_NULL_HANDLE;
    }
    if (offscreenMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, offscreenMemory_, nullptr);
        offscreenMemory_ = VK_NULL_HANDLE;
    }
}

bool VulkanPresenter::ensureStagingBuffer(VkDeviceSize requiredSize) {
    if (stagingBuffer_ != VK_NULL_HANDLE && stagingBufferSize_ >= requiredSize) {
        return true;
    }

    VkDevice device = context_.device();
    if (stagingBuffer_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, stagingBuffer_, nullptr);
        stagingBuffer_ = VK_NULL_HANDLE;
    }
    if (stagingMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, stagingMemory_, nullptr);
        stagingMemory_ = VK_NULL_HANDLE;
    }

    stagingBufferSize_ = requiredSize * 2; // allocate some headroom
    return context_.createBuffer(stagingBufferSize_,
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 stagingBuffer_, stagingMemory_);
}

bool VulkanPresenter::ensureOffscreenImage(uint32_t width, uint32_t height) {
    if (offscreenImage_ != VK_NULL_HANDLE && width_ == width && height_ == height) {
        return true;
    }

    VkDevice device = context_.device();
    if (offscreenView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device, offscreenView_, nullptr);
        offscreenView_ = VK_NULL_HANDLE;
    }
    if (offscreenImage_ != VK_NULL_HANDLE) {
        vkDestroyImage(device, offscreenImage_, nullptr);
        offscreenImage_ = VK_NULL_HANDLE;
    }
    if (offscreenMemory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device, offscreenMemory_, nullptr);
        offscreenMemory_ = VK_NULL_HANDLE;
    }

    width_ = width;
    height_ = height;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (!context_.createImage(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              offscreenImage_, offscreenMemory_)) {
        LOG_ERROR("VulkanPresenter: Failed to create offscreen VkImage");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = offscreenImage_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &offscreenView_) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to create offscreen VkImageView");
        return false;
    }

    return true;
}

bool VulkanPresenter::presentSurface(SkSurface* surface) {
    if (!surface) return false;

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) {
        LOG_ERROR("VulkanPresenter: Failed to peek pixels from SkSurface");
        return false;
    }

    bool isBgra = (pixmap.colorType() == kBGRA_8888_SkColorType);
    return presentPixels(pixmap.addr(),
                         static_cast<uint32_t>(pixmap.width()),
                         static_cast<uint32_t>(pixmap.height()),
                         static_cast<uint32_t>(pixmap.rowBytes()),
                         isBgra);
}

bool VulkanPresenter::presentPixels(const void* pixels, uint32_t width, uint32_t height,
                                    uint32_t stride, bool isBgra)
{
    if (!pixels || width == 0 || height == 0) return false;

    VkDeviceSize imageBytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (!ensureStagingBuffer(imageBytes)) {
        LOG_ERROR("VulkanPresenter: Failed to ensure staging buffer");
        return false;
    }

    VkFormat targetFormat = swapchain_ ? swapchain_->imageFormat() : VK_FORMAT_R8G8B8A8_UNORM;
    bool targetIsBgra = (targetFormat == VK_FORMAT_B8G8R8A8_UNORM || targetFormat == VK_FORMAT_B8G8R8A8_SRGB);
    bool needSwizzle = (isBgra != targetIsBgra);

    uint32_t srcStride = (stride > 0) ? stride : (width * 4);

    void* mapped = nullptr;
    if (vkMapMemory(context_.device(), stagingMemory_, 0, imageBytes, 0, &mapped) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to map staging memory");
        return false;
    }

    const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(pixels);
    uint8_t* dstBytes = reinterpret_cast<uint8_t*>(mapped);

    if (needSwizzle) {
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* rowSrc = srcBytes + y * srcStride;
            uint8_t* rowDst = dstBytes + y * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                rowDst[x * 4 + 0] = rowSrc[x * 4 + 2]; // swap R and B
                rowDst[x * 4 + 1] = rowSrc[x * 4 + 1];
                rowDst[x * 4 + 2] = rowSrc[x * 4 + 0];
                rowDst[x * 4 + 3] = rowSrc[x * 4 + 3];
            }
        }
    } else {
        if (srcStride == width * 4) {
            std::memcpy(dstBytes, srcBytes, imageBytes);
        } else {
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(dstBytes + y * width * 4, srcBytes + y * srcStride, width * 4);
            }
        }
    }

    vkUnmapMemory(context_.device(), stagingMemory_);

    // Headless / Offscreen presentation
    if (!swapchain_) {
        if (!ensureOffscreenImage(width, height)) {
            return false;
        }

        VkCommandBuffer cmd = context_.beginSingleTimeCommands();
        context_.transitionImageLayout(offscreenImage_, VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       cmd);
        context_.copyBufferToImage(stagingBuffer_, offscreenImage_, width, height, cmd);
        context_.transitionImageLayout(offscreenImage_, VK_FORMAT_R8G8B8A8_UNORM,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       cmd);
        context_.endSingleTimeCommands(cmd);
        return true;
    }

    // Windowed presentation via swapchain
    if (swapchain_->extent().width != width || swapchain_->extent().height != height) {
        swapchain_->resize(width, height);
    }

    uint32_t imageIndex = 0;
    SwapchainResult acqResult = swapchain_->acquireNextImage(imageIndex);
    if (acqResult == SwapchainResult::OutOfDate) {
        swapchain_->resize(width, height);
        acqResult = swapchain_->acquireNextImage(imageIndex);
        if (acqResult != SwapchainResult::Success && acqResult != SwapchainResult::Suboptimal) {
            return false;
        }
    } else if (acqResult == SwapchainResult::Error) {
        return false;
    }

    size_t frameIndex = swapchain_->currentFrame();
    VkCommandBuffer cmd = commandBuffers_[frameIndex];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkImage swapImage = swapchain_->image(imageIndex);

    context_.transitionImageLayout(swapImage, swapchain_->imageFormat(),
                                   VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   cmd);

    context_.copyBufferToImage(stagingBuffer_, swapImage, width, height, cmd);

    context_.transitionImageLayout(swapImage, swapchain_->imageFormat(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                   cmd);

    vkEndCommandBuffer(cmd);

    VkSemaphore waitSem = swapchain_->currentImageAvailableSemaphore();
    VkSemaphore signalSem = swapchain_->currentRenderFinishedSemaphore();
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_TRANSFER_BIT };

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &waitSem;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &signalSem;

    if (vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, swapchain_->currentInFlightFence()) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: vkQueueSubmit failed");
        return false;
    }

    SwapchainResult presResult = swapchain_->present(imageIndex);
    if (presResult == SwapchainResult::OutOfDate || presResult == SwapchainResult::Suboptimal) {
        swapchain_->resize(width, height);
    }

    width_ = width;
    height_ = height;
    return true;
}

bool VulkanPresenter::readbackPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    if (offscreenImage_ == VK_NULL_HANDLE || width_ == 0 || height_ == 0) {
        return false;
    }

    VkDeviceSize requiredSize = static_cast<VkDeviceSize>(width_) * height_ * 4;
    VkDevice device = context_.device();

    if (readbackBuffer_ == VK_NULL_HANDLE || readbackBufferSize_ < requiredSize) {
        if (readbackBuffer_ != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, readbackBuffer_, nullptr);
            readbackBuffer_ = VK_NULL_HANDLE;
        }
        if (readbackMemory_ != VK_NULL_HANDLE) {
            vkFreeMemory(device, readbackMemory_, nullptr);
            readbackMemory_ = VK_NULL_HANDLE;
        }
        readbackBufferSize_ = requiredSize * 2;
        if (!context_.createBuffer(readbackBufferSize_,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   readbackBuffer_, readbackMemory_)) {
            LOG_ERROR("VulkanPresenter: Failed to create readback buffer");
            return false;
        }
    }

    VkCommandBuffer cmd = context_.beginSingleTimeCommands();
    context_.copyImageToBuffer(offscreenImage_, readbackBuffer_, width_, height_, cmd);
    context_.endSingleTimeCommands(cmd);

    void* mapped = nullptr;
    if (vkMapMemory(device, readbackMemory_, 0, requiredSize, 0, &mapped) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: Failed to map readback memory");
        return false;
    }

    outPixels.resize(requiredSize);
    std::memcpy(outPixels.data(), mapped, requiredSize);
    vkUnmapMemory(device, readbackMemory_);

    outWidth = width_;
    outHeight = height_;
    return true;
}

} // namespace bro::render
