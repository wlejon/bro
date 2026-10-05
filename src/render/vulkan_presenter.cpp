#include "render/vulkan_presenter.h"
#include "render/pixel_convert.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <include/core/SkColorType.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSurface.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::render {

namespace {

// Clip `layer` to the target and stage it in the target's byte order.
bool stageLayer(VulkanFrames& frames, const PresentPixels& layer, uint32_t targetW, uint32_t targetH,
                bool targetBgra, VkBuffer& buffer, VkBufferImageCopy& region) {
    const uint32_t w = std::min(layer.width, targetW);
    const uint32_t h = std::min(layer.height, targetH);
    UploadSlice staging = frames.allocUpload(static_cast<VkDeviceSize>(w) * h * 4);
    if (!staging) return false;
    copyPixels32(staging.mapped, static_cast<size_t>(w) * 4, layer.pixels,
                 layer.stride ? layer.stride : static_cast<size_t>(layer.width) * 4,
                 w, h, layer.bgra != targetBgra);
    buffer = staging.buffer;
    region = VkBufferImageCopy{};
    region.bufferOffset = staging.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    return true;
}

void transferWriteBarrier(VkCommandBuffer cmd, VkImage image) {
    ImageBarrier waw;
    waw.image = image;
    waw.oldLayout = waw.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    waw.srcStages = waw.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    waw.srcAccess = waw.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    cmdImageBarrier(cmd, waw);
}

} // namespace

PresentPixels VulkanPresenter::surfaceLayer(SkSurface* surface) {
    PresentPixels layer;
    SkPixmap pixmap;
    if (!surface || !surface->peekPixels(&pixmap)) return layer;
    if (pixmap.colorType() != kBGRA_8888_SkColorType && pixmap.colorType() != kRGBA_8888_SkColorType) {
        LOG_ERROR("VulkanPresenter: unsupported surface color type %d", pixmap.colorType());
        return layer;
    }
    layer.pixels = pixmap.addr();
    layer.width = static_cast<uint32_t>(pixmap.width());
    layer.height = static_cast<uint32_t>(pixmap.height());
    layer.stride = static_cast<uint32_t>(pixmap.rowBytes());
    layer.bgra = pixmap.colorType() == kBGRA_8888_SkColorType;
    return layer;
}

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
        width_ = swapchain_->extent().width;
        height_ = swapchain_->extent().height;
    }
    return initBlendResources();
}

void VulkanPresenter::cleanup() {
    if (context_.device() == VK_NULL_HANDLE) return;
    context_.queue().waitIdle();
    destroyBlendResources();
    destroyImageNow(offscreen_);
    if (readbackBuffer_ != VK_NULL_HANDLE) context_.destroyBuffer(readbackBuffer_, readbackAllocId_);
    readbackBuffer_ = VK_NULL_HANDLE;
    readbackAllocId_ = 0;
    readbackMapped_ = nullptr;
    readbackSize_ = 0;
    readbackTicket_ = 0;
}

bool VulkanPresenter::ensureImage(Image& img, uint32_t width, uint32_t height, VkFormat format,
                                  VkImageUsageFlags usage) {
    if (img.image != VK_NULL_HANDLE && img.width == width && img.height == height && img.format == format)
        return true;
    retireImage(img);

    if (!context_.createImage(width, height, format, VK_IMAGE_TILING_OPTIMAL, usage,
                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                              img.image, img.memory, img.offset, img.allocId)) {
        LOG_ERROR("VulkanPresenter: failed to create a %ux%u image", width, height);
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = img.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = colorRange();
    if (vkCreateImageView(context_.device(), &viewInfo, nullptr, &img.view) != VK_SUCCESS) {
        LOG_ERROR("VulkanPresenter: failed to create an image view");
        destroyImageNow(img);
        return false;
    }
    img.format = format;
    img.width = width;
    img.height = height;
    img.lastUseSerial = 0;
    return true;
}

// Release an image the GPU may still be using: destroyed once the frames
// submitted so far have completed.
void VulkanPresenter::retireImage(Image& img) {
    if (img.image == VK_NULL_HANDLE && img.view == VK_NULL_HANDLE) return;
    VulkanContext* ctx = &context_;
    Image dead = img;
    context_.frames().defer([ctx, dead]() {
        if (dead.view != VK_NULL_HANDLE) vkDestroyImageView(ctx->device(), dead.view, nullptr);
        ctx->destroyImage(dead.image, dead.allocId);
    });
    img = Image{};
}

void VulkanPresenter::destroyImageNow(Image& img) {
    if (img.view != VK_NULL_HANDLE) vkDestroyImageView(context_.device(), img.view, nullptr);
    if (img.image != VK_NULL_HANDLE || img.allocId != 0) context_.destroyImage(img.image, img.allocId);
    img = Image{};
}

bool VulkanPresenter::ensureReadbackBuffer(VkDeviceSize size) {
    if (readbackBuffer_ != VK_NULL_HANDLE && readbackSize_ >= size) return true;
    if (readbackBuffer_ != VK_NULL_HANDLE) {
        VulkanContext* ctx = &context_;
        VkBuffer buf = readbackBuffer_;
        uint64_t id = readbackAllocId_;
        context_.frames().defer([ctx, buf, id]() { ctx->destroyBuffer(buf, id); });
    }
    readbackBuffer_ = VK_NULL_HANDLE;
    readbackAllocId_ = 0;
    readbackMapped_ = nullptr;
    readbackSize_ = size;
    if (!context_.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               readbackBuffer_, readbackMemory_, readbackOffset_, readbackAllocId_,
                               readbackMapped_) || !readbackMapped_) {
        LOG_ERROR("VulkanPresenter: failed to create the readback buffer");
        readbackSize_ = 0;
        return false;
    }
    return true;
}

bool VulkanPresenter::present(const PresentFrame& frame) {
    // An empty frame is just the clear color, given a size to clear.
    if (!frame.below && !frame.hasImages() && !swapchain_ && (frame.width == 0 || frame.height == 0)) return false;
    return swapchain_ ? presentToSwapchain(frame) : presentOffscreen(frame);
}

// Record the frame into `target`, reporting the layout it is left in
// (TRANSFER_DST, or COLOR_ATTACHMENT after blending) for the caller to move
// on from — also when recording fails part-way. `acquireStages` are the stages
// a prior user of the target (the swapchain acquire, the last readback) is
// ordered against.
bool VulkanPresenter::recordFrame(VkCommandBuffer cmd, const PresentFrame& frame, const Target& target,
                                  VkPipelineStageFlags acquireStages, VkImageLayout& targetLayout) {
    ImageBarrier toDst;
    toDst.image = target.image;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcStages = acquireStages;
    toDst.dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    toDst.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    cmdImageBarrier(cmd, toDst);
    targetLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    // Images sampled in place were last written by their producer's earlier
    // submission (a Skia flush leaves them SHADER_READ_ONLY); make those
    // writes visible to the copies and draws below.
    for (const PresentImage& image : frame.images) {
        if (!image || image.view == VK_NULL_HANDLE) continue;
        ImageBarrier acquire;
        acquire.image = image.image;
        acquire.oldLayout = acquire.newLayout = image.layout;
        acquire.srcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        acquire.srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        acquire.dstStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        acquire.dstAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        cmdImageBarrier(cmd, acquire);
    }

    // The base is the layer below, or else the first image copied straight
    // in when it lands 1:1 at the top-left uncut — a copy only equals
    // blending it when what it would blend over is transparent.
    const PresentImage* first = frame.images.empty() ? nullptr : &frame.images.front();
    const bool clearIsTransparent = frame.clearColor[0] == 0.0f && frame.clearColor[1] == 0.0f &&
                                    frame.clearColor[2] == 0.0f && frame.clearColor[3] == 0.0f;
    const bool imageIsBase = !frame.below && clearIsTransparent && first && *first && !first->clipped &&
                             first->dstX == 0.0f && first->dstY == 0.0f &&
                             first->dstW == static_cast<float>(first->width) &&
                             first->dstH == static_cast<float>(first->height);
    const uint32_t baseW = frame.below ? frame.below.width : imageIsBase ? first->width : 0;
    const uint32_t baseH = frame.below ? frame.below.height : imageIsBase ? first->height : 0;
    if (baseW < target.width || baseH < target.height) {
        VkClearColorValue clear{};
        std::memcpy(clear.float32, frame.clearColor, sizeof(clear.float32));
        const VkImageSubresourceRange range = colorRange();
        vkCmdClearColorImage(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        transferWriteBarrier(cmd, target.image);
    }

    if (frame.below) {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkBufferImageCopy region{};
        if (!stageLayer(context_.frames(), frame.below, target.width, target.height,
                        isBgraFormat(target.format), buffer, region))
            return false;
        vkCmdCopyBufferToImage(cmd, buffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    if (imageIsBase) {
        const uint32_t w = std::min(first->width, target.width);
        const uint32_t h = std::min(first->height, target.height);
        const bool toSrc = first->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        if (toSrc)
            cmdTransitionImage(cmd, first->image, colorRange(), first->layout,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {static_cast<int32_t>(w), static_cast<int32_t>(h), 1};
        vkCmdBlitImage(cmd, first->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        if (toSrc)
            cmdTransitionImage(cmd, first->image, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               first->layout);
    }

    // Blended layers, in order: each image (but a base one), then the CPU
    // layer above it. Every copy and upload is recorded before the one pass
    // that draws them.
    std::vector<BlendDraw> draws;
    draws.reserve(frame.images.size() * 2);
    for (size_t i = 0; i < frame.images.size(); ++i) {
        const PresentImage& image = frame.images[i];
        if (image && !(i == 0 && imageIsBase)) {
            BlendDraw& draw = draws.emplace_back();
            const bool described = image.view != VK_NULL_HANDLE ? describeInPlace(image, target, draw)
                                                                 : copyImageTexture(cmd, image, i, target, draw);
            if (!described) return false;
        }
        if (image.above) {
            if (!uploadLayerTexture(cmd, image.above, i, target, draws.emplace_back())) return false;
        }
    }
    if (draws.empty()) return true;

    // Blending reads what the transfers above wrote.
    cmdTransitionImage(cmd, target.image, colorRange(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    targetLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    recordBlendDraws(cmd, target, draws);
    return true;
}

bool VulkanPresenter::presentToSwapchain(const PresentFrame& frame) {
    auto& frames = context_.frames();
    frames.ensureFrame();

    uint32_t imageIndex = 0;
    SwapchainResult acquired = swapchain_->acquire(imageIndex);
    if (acquired == SwapchainResult::Minimized) return true;
    if (acquired != SwapchainResult::Success && acquired != SwapchainResult::Suboptimal) return false;

    width_ = swapchain_->extent().width;
    height_ = swapchain_->extent().height;
    const Target target{swapchain_->image(imageIndex), swapchain_->imageView(imageIndex),
                        swapchain_->imageFormat(), width_, height_};

    // The acquire semaphore is waited on at the first stage that touches the
    // image; the layout transition is ordered after it through the same stages.
    constexpr VkPipelineStageFlags kAcquireStages =
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkCommandBuffer cmd = frames.beginCommands();
    if (cmd == VK_NULL_HANDLE) return false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    const bool recorded = recordFrame(cmd, frame, target, kAcquireStages, layout);

    // Whatever the recording left the image in, it ends ready to present: a
    // failed recording is still submitted, to consume the acquire semaphore.
    cmdTransitionImage(cmd, target.image, colorRange(), layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    const uint64_t ticket = frames.submit(
        cmd, {{swapchain_->acquireSemaphore(), kAcquireStages, 0}},
        {{swapchain_->presentSemaphore(imageIndex), 0}});
    if (ticket == 0) return false;
    const SwapchainResult presented = swapchain_->present(imageIndex, ticket);
    return recorded && presented != SwapchainResult::Error;
}

bool VulkanPresenter::presentOffscreen(const PresentFrame& frame) {
    auto& frames = context_.frames();
    frames.ensureFrame();

    // The target is the frame the CPU layers make up; a frame of GPU images
    // alone covers them all.
    uint32_t w = frame.below ? frame.below.width : 0u;
    uint32_t h = frame.below ? frame.below.height : 0u;
    for (const PresentImage& image : frame.images) {
        if (!image.above) continue;
        w = std::max(w, image.above.width);
        h = std::max(h, image.above.height);
    }
    if (frame.width > 0 && frame.height > 0) {
        w = frame.width;
        h = frame.height;
    } else if (w == 0 || h == 0) {
        for (const PresentImage& image : frame.images) {
            if (!image) continue;
            w = std::max(w, static_cast<uint32_t>(std::ceil(std::max(0.0f, image.dstX + image.dstW))));
            h = std::max(h, static_cast<uint32_t>(std::ceil(std::max(0.0f, image.dstY + image.dstH))));
        }
    }
    if (w == 0 || h == 0) return false;
    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    readbackTicket_ = 0;
    if (!ensureImage(offscreen_, w, h, VK_FORMAT_R8G8B8A8_UNORM, kUsage)) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    if (!ensureReadbackBuffer(bytes)) return false;
    width_ = w;
    height_ = h;
    const Target target{offscreen_.image, offscreen_.view, offscreen_.format, w, h};

    VkCommandBuffer cmd = frames.beginCommands();
    if (cmd == VK_NULL_HANDLE) return false;
    // The previous present's readback copy read this image (TRANSFER).
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    const bool recorded = recordFrame(cmd, frame, target, VK_PIPELINE_STAGE_TRANSFER_BIT, layout);
    cmdTransitionImage(cmd, target.image, colorRange(), layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readbackBuffer_, 1, &region);

    VkBufferMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = readbackBuffer_;
    toHost.size = bytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &toHost, 0, nullptr);

    const uint64_t ticket = frames.submit(cmd);
    if (ticket == 0 || !recorded) return false;
    readbackTicket_ = ticket;
    return true;
}

bool VulkanPresenter::readbackPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight) {
    if (swapchain_ || readbackTicket_ == 0 || !readbackMapped_) return false;
    if (!context_.queue().wait(readbackTicket_)) return false;
    const size_t bytes = static_cast<size_t>(width_) * height_ * 4;
    outPixels.resize(bytes);
    std::memcpy(outPixels.data(), readbackMapped_, bytes);
    outWidth = width_;
    outHeight = height_;
    return true;
}

bool VulkanPresenter::presentSurface(SkSurface* surface) {
    PresentFrame frame;
    frame.below = surfaceLayer(surface);
    return present(frame);
}

bool VulkanPresenter::presentPixels(const void* pixels, uint32_t width, uint32_t height,
                                    uint32_t stride, bool isBgra) {
    PresentFrame frame;
    frame.below = PresentPixels{pixels, width, height, stride, isBgra};
    return present(frame);
}

bool VulkanPresenter::presentImage(VkImage image, uint32_t width, uint32_t height,
                                   VkImageLayout currentLayout, SkSurface* overlaySurface) {
    PresentFrame frame;
    PresentImage& layer = frame.images.emplace_back(PresentImage::at1to1(image, currentLayout, width, height));
    if (overlaySurface) layer.above = surfaceLayer(overlaySurface);
    return present(frame);
}

} // namespace bro::render
