#pragma once

#include "render/vulkan_context.h"
#include "webgl/webgl_types.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::webgl::vk {

/// Manages the offscreen canvas render target (VkImage color + depth/stencil attachments),
/// resizing, layout transitions, zero-copy presentation, and staging pixel readback.
/// The context's attributes decide the attachments: a depth and/or stencil
/// image only when asked for, and without alpha the color image keeps its
/// alpha at one (the context draws it as an RGB format: alphaOne).
class WebGLVkCanvas {
public:
    explicit WebGLVkCanvas(render::VulkanContext& context, const ContextAttributes& attributes = {});
    ~WebGLVkCanvas();

    WebGLVkCanvas(const WebGLVkCanvas&) = delete;
    WebGLVkCanvas& operator=(const WebGLVkCanvas&) = delete;

    /// Create color and depth/stencil attachments of the given size. Their
    /// contents are undefined until recordInit() clears them.
    bool init(uint32_t width, uint32_t height);

    /// Recreate the attachments at a new size (contents cleared by recordInit()).
    bool resize(uint32_t width, uint32_t height);

    /// Record the clear of freshly created attachments into `cmd` (no-op once done).
    void recordInit(VkCommandBuffer cmd);

    /// Record a clear of every attachment to GL's defaults (transparent
    /// black, or opaque without alpha; depth 1; stencil 0), after whatever
    /// last used them, outside a render pass.
    void recordClear(VkCommandBuffer cmd);

    /// No alpha channel: alpha reads, and is kept, as one.
    bool opaque() const { return !attributes_.alpha; }

    /// Release the images; they are destroyed once the GPU is done with them.
    void cleanup();

    /// Transition color attachment image to new layout.
    void transitionColor(VkCommandBuffer cmd, VkImageLayout newLayout);

    /// Transition depth attachment image to new layout.
    void transitionDepth(VkCommandBuffer cmd, VkImageLayout newLayout);

    /// Transition color image to SHADER_READ_ONLY_OPTIMAL for zero-copy UI compositing.
    void transitionToShaderRead(VkCommandBuffer cmd) {
        transitionColor(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    /// Transition color image to COLOR_ATTACHMENT_OPTIMAL for WebGL rendering.
    void transitionToColorAttachment(VkCommandBuffer cmd) {
        transitionColor(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }

    /// Record a copy of the color attachment into `dst` as tightly packed,
    /// top-down RGBA (width x height x 4), leaving the layout as it was.
    bool recordCopy(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset = 0);

    // Accessors
    VkImage colorImage() const { return colorImage_; }
    VkImageView colorView() const { return colorView_; }
    /// The view the compositor samples: the color view, or without alpha one
    /// that reads alpha as one (whatever a blit left there).
    VkImageView presentView() const { return presentView_ != VK_NULL_HANDLE ? presentView_ : colorView_; }
    VkFormat colorFormat() const { return colorFormat_; }
    VkImageLayout colorLayout() const { return colorLayout_; }

    VkImage depthImage() const { return depthImage_; }
    VkImageView depthView() const { return depthView_; }
    VkFormat depthFormat() const { return depthFormat_; }
    VkImageLayout depthLayout() const { return depthLayout_; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool isValid() const { return colorImage_ != VK_NULL_HANDLE; }

    render::VulkanContext& context() { return context_; }

private:
    bool createColorAttachment(uint32_t width, uint32_t height);
    bool createDepthAttachment(uint32_t width, uint32_t height);
    void clearImages(VkCommandBuffer cmd);

    render::VulkanContext& context_;
    ContextAttributes attributes_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    VkImage colorImage_ = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory_ = VK_NULL_HANDLE;
    VkDeviceSize colorOffset_ = 0;
    uint64_t colorAllocId_ = 0;
    VkImageView colorView_ = VK_NULL_HANDLE;
    VkImageView presentView_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkDeviceSize depthOffset_ = 0;
    uint64_t depthAllocId_ = 0;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;  // UNDEFINED: neither depth nor stencil
    VkImageLayout depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    bool needsInit_ = false;
};

} // namespace bro::webgl::vk
