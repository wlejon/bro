#pragma once

// Small Vulkan helpers shared by the presenter, the 3D scene and WebGL:
// memory-type lookup, image aspect/format queries, and image layout barriers
// whose stage and access masks follow from the layouts involved.

#include <vulkan/vulkan.h>
#include <cstdint>
#include <optional>

namespace bro::render {

/// The first memory type allowed by `typeBits` that has every flag in `required`,
/// or nullopt when the device has none.
std::optional<uint32_t> findMemoryType(const VkPhysicalDeviceMemoryProperties& props,
                                       uint32_t typeBits, VkMemoryPropertyFlags required);

/// Depth, depth+stencil, stencil or color aspect for an image of `format`.
VkImageAspectFlags imageAspectFor(VkFormat format);

/// True for the 8-bit BGRA formats a swapchain may hand out.
bool isBgraFormat(VkFormat format);

/// The pipeline stages and accesses an image in `layout` is used by. As the
/// source of a barrier only the writes need making available; reads report an
/// empty access mask (`asSource` = true).
struct LayoutUsage {
    VkPipelineStageFlags stages = 0;
    VkAccessFlags access = 0;
};
LayoutUsage layoutUsage(VkImageLayout layout, bool asSource);

/// One image memory barrier with explicit masks.
struct ImageBarrier {
    VkImage image = VK_NULL_HANDLE;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout newLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags srcStages = 0;
    VkAccessFlags srcAccess = 0;
    VkPipelineStageFlags dstStages = 0;
    VkAccessFlags dstAccess = 0;
};
void cmdImageBarrier(VkCommandBuffer cmd, const ImageBarrier& barrier);

/// Transition `range` of `image` from `oldLayout` to `newLayout`, deriving the
/// stage/access masks of both sides from the layouts (see layoutUsage).
void cmdTransitionImage(VkCommandBuffer cmd, VkImage image,
                        const VkImageSubresourceRange& range,
                        VkImageLayout oldLayout, VkImageLayout newLayout);

/// A global memory barrier: `srcAccess` writes by `srcStages` made visible
/// to `dstAccess` in `dstStages`.
void cmdMemoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStages, VkAccessFlags srcAccess,
                      VkPipelineStageFlags dstStages, VkAccessFlags dstAccess);

/// A barrier on `size` bytes of `buffer` at `offset` (VK_WHOLE_SIZE for all).
void cmdBufferBarrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                      VkPipelineStageFlags srcStages, VkAccessFlags srcAccess,
                      VkPipelineStageFlags dstStages, VkAccessFlags dstAccess);

/// Whole-mip-0, single-layer color subresource range.
inline VkImageSubresourceRange colorRange(uint32_t levels = 1, uint32_t layers = 1) {
    return VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
}

} // namespace bro::render
