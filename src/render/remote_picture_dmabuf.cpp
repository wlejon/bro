// DmabufPictureBridge (remote_picture.h): Linux DMA-BUF decoded pictures
// imported into Vulkan images via brodmabuf.

#include "render/remote_picture.h"
#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#if defined(__linux__) && defined(BRO_WITH_DMABUF) && BRO_WITH_DMABUF
#include <brodmabuf/buffer.h>
#include <brodmabuf/vulkan.h>
#include <unistd.h>
#endif

#include <mutex>

namespace bro::render {

#if defined(__linux__) && defined(BRO_WITH_DMABUF) && BRO_WITH_DMABUF

namespace {

struct DmabufSlot {
    VkDevice device = VK_NULL_HANDLE;
    std::unique_ptr<brodmabuf::VulkanImage> vkImage;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    std::shared_ptr<void> keepalive;

    ~DmabufSlot() {
        if (view != VK_NULL_HANDLE && device != VK_NULL_HANDLE) {
            vkDestroyImageView(device, view, nullptr);
        }
        vkImage.reset();
        keepalive.reset();
    }
};

}  // namespace

struct DmabufPictureBridge::Impl {
    VulkanContext* context = nullptr;
    std::unique_ptr<brodmabuf::VulkanContext> dmabufCtx;
    std::mutex mutex;
};

DmabufPictureBridge::DmabufPictureBridge(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DmabufPictureBridge::~DmabufPictureBridge() = default;

std::shared_ptr<DmabufPictureBridge> DmabufPictureBridge::create(VulkanContext& context, std::string* err) {
    if (!context.isValid()) {
        if (err) *err = "Vulkan context is not valid";
        return nullptr;
    }
    const uint32_t queueFamily = context.queueFamilies().graphicsFamily >= 0
        ? static_cast<uint32_t>(context.queueFamilies().graphicsFamily)
        : 0u;
    auto dmabufCtx = brodmabuf::VulkanContext::wrap(
        context.instance(),
        context.physicalDevice(),
        context.device(),
        context.queue().queue(),
        queueFamily);
    if (!dmabufCtx) {
        if (err) *err = "Vulkan device lacks DMA-BUF external memory extensions";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->context = &context;
    impl->dmabufCtx = std::move(dmabufCtx);
    return std::shared_ptr<DmabufPictureBridge>(new DmabufPictureBridge(std::move(impl)));
}

std::shared_ptr<const BridgedPicture> DmabufPictureBridge::importPicture(
    uint32_t drmFormat, uint64_t modifier,
    uint32_t width, uint32_t height,
    uint32_t planeCount, const DmabufPlaneDesc* planes,
    std::shared_ptr<void> keepalive, std::string* err) {
    if (!impl_ || !impl_->dmabufCtx || !impl_->context) {
        if (err) *err = "DmabufPictureBridge is not initialized";
        return nullptr;
    }
    if (width == 0 || height == 0 || planeCount == 0 || !planes || planes[0].fd < 0) {
        if (err) *err = "invalid DMA-BUF picture dimensions or plane descriptor";
        return nullptr;
    }
    // Multi-plane NV12 requires VkSamplerYcbcrConversion; reject here so the
    // caller falls back to PictureMemory::Cpu (or CPU NV12->RGBA upload).
    if (planeCount > 1) {
        if (err) *err = "multi-plane DMA-BUF (NV12) requires sampler YCbCr conversion; using CPU fallback";
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    brodmabuf::DmaBufAttributes attrs;
    attrs.width = width;
    attrs.height = height;
    attrs.drm_format = drmFormat;
    attrs.modifier = modifier;
    for (uint32_t i = 0; i < planeCount && i < 4; ++i) {
        if (planes[i].fd >= 0) {
            attrs.planes.emplace_back(
                brodmabuf::UniqueFd(::dup(planes[i].fd)),
                planes[i].pitch,
                planes[i].offset);
        }
    }

    auto importRes = impl_->dmabufCtx->import_dmabuf(attrs);
    if (!importRes) {
        if (err) *err = importRes.error().message;
        return nullptr;
    }
    auto vkImg = std::move(importRes.value());

    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = vkImg->handle();
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = vkImg->format();
    viewInfo.subresourceRange = colorRange();

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(impl_->context->device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        if (err) *err = "vkCreateImageView failed for imported DMA-BUF";
        return nullptr;
    }

    auto slot = std::make_shared<DmabufSlot>();
    slot->device = impl_->context->device();
    slot->format = vkImg->format();
    slot->width = width;
    slot->height = height;
    slot->view = view;
    slot->vkImage = std::move(vkImg);
    slot->keepalive = std::move(keepalive);

    const uint64_t serial = imports_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto pic = std::make_shared<BridgedPicture>();
    pic->slot = std::move(slot);
    pic->width = width;
    pic->height = height;
    pic->serial = serial;
    return pic;
}

LayerImage DmabufPictureBridge::image(const BridgedPicture& picture) {
    LayerImage out;
    if (!picture.slot) return out;
    auto* slot = static_cast<const DmabufSlot*>(picture.slot.get());
    if (!slot->vkImage || slot->view == VK_NULL_HANDLE) return out;
    out.image = slot->vkImage->handle();
    out.view = slot->view;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.format = slot->format;
    out.width = slot->width;
    out.height = slot->height;
    return out;
}

#endif  // defined(__linux__) && defined(BRO_WITH_DMABUF) && BRO_WITH_DMABUF

}  // namespace bro::render
