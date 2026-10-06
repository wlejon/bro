#include "render/vulkan_dmabuf_importer.h"
#include "render/vulkan_context.h"
#include <unistd.h>

namespace bro::render {

VulkanDmabufImporter::VulkanDmabufImporter(VulkanContext& context)
    : context_(context) {}

VulkanDmabufImporter::~VulkanDmabufImporter() {
    clear();
}

bool VulkanDmabufImporter::init() {
#if defined(__linux__)
    if (!context_.isValid()) return false;
    uint32_t queueFamily = context_.queueFamilies().graphicsFamily >= 0
        ? static_cast<uint32_t>(context_.queueFamilies().graphicsFamily)
        : 0;
    dmabufCtx_ = brodmabuf::VulkanContext::wrap(
        context_.instance(),
        context_.physicalDevice(),
        context_.device(),
        context_.queue().queue(),
        queueFamily);
    initialized_ = (dmabufCtx_ != nullptr);
    return initialized_;
#else
    return false;
#endif
}

ImportedClientBuffer* VulkanDmabufImporter::getOrImport(
    const DmabufLayerSource& src, uint64_t frameSerial) {
    if (src.bufferId == 0) return nullptr;

    // Honor explicit sync fence if provided
#if defined(__linux__)
    if (src.syncFd >= 0) {
        brodmabuf::SyncFile fence(brodmabuf::UniqueFd(::dup(src.syncFd)));
        (void)fence.wait(/*timeout_ms=*/50);
    }
#endif

    auto it = cache_.find(src.bufferId);
    if (it != cache_.end()) {
        it->second->lastUsedFrame = frameSerial;
        return it->second.get();
    }

#if defined(__linux__)
    if (!initialized_ || !dmabufCtx_) return nullptr;

    brodmabuf::DmaBufAttributes attrs;
    attrs.width = src.width;
    attrs.height = src.height;
    attrs.drm_format = src.drmFormat;
    attrs.modifier = src.modifier;
    for (uint32_t i = 0; i < src.planeCount && i < 4; ++i) {
        if (src.fds[i] >= 0) {
            attrs.planes.emplace_back(
                brodmabuf::UniqueFd(::dup(src.fds[i])),
                src.strides[i],
                src.offsets[i]);
        }
    }

    auto importRes = dmabufCtx_->import_dmabuf(attrs);
    if (!importRes) return nullptr;

    auto vkImg = std::move(importRes.value());
    VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = vkImg->handle(),
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = vkImg->format(),
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        }
    };

    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(context_.device(), &viewInfo, nullptr, &view) != VK_SUCCESS) {
        return nullptr;
    }

    auto buf = std::make_unique<ImportedClientBuffer>();
    buf->bufferId = src.bufferId;
    buf->image = vkImg->handle();
    buf->view = view;
    buf->width = src.width;
    buf->height = src.height;
    buf->lastUsedFrame = frameSerial;
    buf->vulkanImage = std::move(vkImg);

    auto* ptr = buf.get();
    cache_[src.bufferId] = std::move(buf);
    return ptr;
#else
    (void)src;
    (void)frameSerial;
    return nullptr;
#endif
}

void VulkanDmabufImporter::prune(uint64_t currentFrameSerial, uint64_t maxAgeFrames) {
    for (auto it = cache_.begin(); it != cache_.end();) {
        if (currentFrameSerial > it->second->lastUsedFrame &&
            (currentFrameSerial - it->second->lastUsedFrame) > maxAgeFrames) {
            if (it->second->view != VK_NULL_HANDLE) {
                vkDestroyImageView(context_.device(), it->second->view, nullptr);
            }
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void VulkanDmabufImporter::releaseBuffer(uint64_t bufferId) {
    auto it = cache_.find(bufferId);
    if (it != cache_.end()) {
        if (it->second->view != VK_NULL_HANDLE) {
            vkDestroyImageView(context_.device(), it->second->view, nullptr);
        }
        cache_.erase(it);
    }
}

void VulkanDmabufImporter::clear() {
    for (auto& [id, buf] : cache_) {
        if (buf && buf->view != VK_NULL_HANDLE) {
            vkDestroyImageView(context_.device(), buf->view, nullptr);
            buf->view = VK_NULL_HANDLE;
        }
    }
    cache_.clear();
}

} // namespace bro::render
