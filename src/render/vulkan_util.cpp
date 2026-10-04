#include "render/vulkan_util.h"

namespace bro::render {

std::optional<uint32_t> findMemoryType(const VkPhysicalDeviceMemoryProperties& props,
                                       uint32_t typeBits, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return std::nullopt;
}

VkImageAspectFlags imageAspectFor(VkFormat format) {
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

bool isBgraFormat(VkFormat format) {
    return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}

LayoutUsage layoutUsage(VkImageLayout layout, bool asSource) {
    constexpr VkPipelineStageFlags kShaderStages =
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags kDepthStages =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;

    LayoutUsage u;
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
    case VK_IMAGE_LAYOUT_PREINITIALIZED:
        u.stages = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        u.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        u.access = asSource ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                            : VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
        u.stages = kDepthStages;
        u.access = asSource ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                            : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
        u.stages = kDepthStages | kShaderStages;
        u.access = asSource ? 0
                            : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        u.stages = kShaderStages;
        u.access = asSource ? 0 : VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        u.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.access = asSource ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        u.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
        u.access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        // Presentation is ordered by the semaphores around vkQueuePresentKHR;
        // the barrier only has to complete before the submission signals.
        u.stages = asSource ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        break;
    default:
        // GENERAL (storage images) and anything exotic: every stage, every access.
        u.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        u.access = asSource ? VK_ACCESS_MEMORY_WRITE_BIT
                            : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        break;
    }
    return u;
}

void cmdImageBarrier(VkCommandBuffer cmd, const ImageBarrier& b) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = b.srcAccess;
    barrier.dstAccessMask = b.dstAccess;
    barrier.oldLayout = b.oldLayout;
    barrier.newLayout = b.newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = b.image;
    barrier.subresourceRange = b.range;
    vkCmdPipelineBarrier(cmd, b.srcStages, b.dstStages, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void cmdTransitionImage(VkCommandBuffer cmd, VkImage image,
                        const VkImageSubresourceRange& range,
                        VkImageLayout oldLayout, VkImageLayout newLayout) {
    const LayoutUsage src = layoutUsage(oldLayout, /*asSource=*/true);
    const LayoutUsage dst = layoutUsage(newLayout, /*asSource=*/false);
    ImageBarrier b;
    b.image = image;
    b.range = range;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcStages = src.stages;
    b.srcAccess = src.access;
    b.dstStages = dst.stages;
    b.dstAccess = dst.access;
    cmdImageBarrier(cmd, b);
}

} // namespace bro::render
