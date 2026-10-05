// What a sampler reads when no complete texture is bound to its unit: GL
// returns (0, 0, 0, 1). Vulkan needs a real descriptor of the right view type
// and component kind, so the context keeps 1x1 images of each; a shadow
// sampler reads a depth image through a comparison that always fails.

#include "webgl/vulkan/webgl_vk_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

namespace bro::webgl::vk {

namespace {

constexpr VkFormat kPlaceholderFormats[4] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_D16_UNORM,
};
constexpr size_t kDepthPlaceholder = 3;

bool createImage(render::VulkanContext& context, VkImageType type, VkFormat format, uint32_t layers,
                 VkImageCreateFlags flags, VkImage& image, VkDeviceMemory& memory) {
    VkDevice dev = context.device();
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.flags = flags;
    info.imageType = type;
    info.format = format;
    info.extent = {1, 1, 1};
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &info, nullptr, &image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = context.findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == render::VulkanContext::kNoMemoryType ||
        vkAllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyImage(dev, image, nullptr);
        image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(dev, image, memory, 0);
    return true;
}

VkImageView createView(VkDevice dev, VkImage image, VkImageViewType type, VkFormat format, uint32_t layers) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = type;
    info.format = format;
    info.subresourceRange = {render::imageAspectFor(format), 0, 1, 0, layers};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(dev, &info, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

void recordClear(VkCommandBuffer cmd, VkImage image, VkFormat format, uint32_t layers, const VkClearColorValue& value) {
    const VkImageSubresourceRange range{render::imageAspectFor(format), 0, 1, 0, layers};
    render::cmdTransitionImage(cmd, image, range, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    } else {
        const VkClearDepthStencilValue depth{0.0f, 0};
        vkCmdClearDepthStencilImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &range);
    }
    render::cmdTransitionImage(cmd, image, range, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

} // namespace

void WebGLVkContext::createPlaceholders() {
    VkDevice dev = context_.device();
    VkCommandBuffer cmd = transferCommands();
    for (size_t kind = 0; kind < placeholders_.size(); ++kind) {
        Placeholder& p = placeholders_[kind];
        const VkFormat format = kPlaceholderFormats[kind];
        VkClearColorValue value{};
        if (kind == 0) value.float32[3] = 1.0f;
        else value.uint32[3] = 1;
        const bool depth = kind == kDepthPlaceholder;  // no 3D shadow samplers
        if (!createImage(context_, VK_IMAGE_TYPE_2D, format, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, p.layered,
                         p.layeredMemory) ||
            (!depth && !createImage(context_, VK_IMAGE_TYPE_3D, format, 1, 0, p.volume, p.volumeMemory))) {
            LOG_ERROR("WebGLVkContext: failed to create placeholder textures");
            continue;
        }
        p.views[0] = createView(dev, p.layered, VK_IMAGE_VIEW_TYPE_2D, format, 1);
        p.views[1] = createView(dev, p.layered, VK_IMAGE_VIEW_TYPE_2D_ARRAY, format, 1);
        p.views[2] = createView(dev, p.layered, VK_IMAGE_VIEW_TYPE_CUBE, format, 6);
        recordClear(cmd, p.layered, format, 6, value);
        if (!depth) {
            p.views[3] = createView(dev, p.volume, VK_IMAGE_VIEW_TYPE_3D, format, 1);
            recordClear(cmd, p.volume, format, 1, value);
        }
    }

    // Nearest filtering: an integer view cannot be sampled linearly.
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sampler, nullptr, &placeholderSampler_) != VK_SUCCESS)
        LOG_ERROR("WebGLVkContext: failed to create the placeholder sampler");
    // Only where shadow samplers can link (see linkProgram).
    sampler.compareEnable = VK_TRUE;
    sampler.compareOp = VK_COMPARE_OP_NEVER;
    if (context_.comparisonSamplers() &&
        vkCreateSampler(dev, &sampler, nullptr, &placeholderShadowSampler_) != VK_SUCCESS)
        LOG_ERROR("WebGLVkContext: failed to create the placeholder shadow sampler");
}

void WebGLVkContext::destroyPlaceholders() {
    VkDevice dev = context_.device();
    for (Placeholder& p : placeholders_) {
        for (VkImageView view : p.views)
            if (view != VK_NULL_HANDLE) vkDestroyImageView(dev, view, nullptr);
        if (p.layered != VK_NULL_HANDLE) vkDestroyImage(dev, p.layered, nullptr);
        if (p.volume != VK_NULL_HANDLE) vkDestroyImage(dev, p.volume, nullptr);
        if (p.layeredMemory != VK_NULL_HANDLE) vkFreeMemory(dev, p.layeredMemory, nullptr);
        if (p.volumeMemory != VK_NULL_HANDLE) vkFreeMemory(dev, p.volumeMemory, nullptr);
        p = Placeholder{};
    }
    if (placeholderSampler_ != VK_NULL_HANDLE) vkDestroySampler(dev, placeholderSampler_, nullptr);
    if (placeholderShadowSampler_ != VK_NULL_HANDLE) vkDestroySampler(dev, placeholderShadowSampler_, nullptr);
    placeholderSampler_ = VK_NULL_HANDLE;
    placeholderShadowSampler_ = VK_NULL_HANDLE;
}

VkImageView WebGLVkContext::placeholderView(GLenum samplerType) const {
    size_t kind = 0;
    switch (samplerType) {
        case GL_INT_SAMPLER_2D: case GL_INT_SAMPLER_3D: case GL_INT_SAMPLER_CUBE: case GL_INT_SAMPLER_2D_ARRAY:
            kind = 1;
            break;
        case GL_UNSIGNED_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_CUBE:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            kind = 2;
            break;
        case GL_SAMPLER_2D_SHADOW: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_SAMPLER_CUBE_SHADOW:
            kind = kDepthPlaceholder;
            break;
        default: break;
    }
    size_t view = 0;
    switch (samplerType) {
        case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY:
        case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
            view = 1;
            break;
        case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE:
            view = 2;
            break;
        case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D:
            view = 3;
            break;
        default: break;
    }
    return placeholders_[kind].views[view];
}

} // namespace bro::webgl::vk
