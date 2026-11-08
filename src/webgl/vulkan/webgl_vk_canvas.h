#pragma once

#include "render/layer_image.h"
#include "render/vulkan_context.h"
#include "webgl/webgl_types.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::webgl::vk {

class WebGLVkStream;

/// Manages the offscreen canvas render target (VkImage color + depth/stencil attachments),
/// resizing, layout transitions, zero-copy presentation, and staging pixel readback.
/// The context's attributes decide the attachments: a depth and/or stencil
/// image only when asked for, and without alpha the color image keeps its
/// alpha at one (the context draws it as an RGB format: alphaOne).
///
/// The compositor never samples the drawing buffer itself: each frame's end
/// copies it into a second color image, the front (recordPresentCopy), which
/// is what the presenter samples. So the context can go on drawing the next
/// frame into the drawing buffer while the presenter's submission for the
/// last one is still to come: the front changes only at a frame's end, in
/// one submission, which queue order puts wholly before or wholly after any
/// presenter submission sampling it. The front stays SHADER_READ_ONLY_OPTIMAL
/// between copies.
///
/// Replaced images are destroyed through `stream` (the context's, whose
/// commands draw into them), once its work so far is done and, since the
/// presenter samples the front, once the queue ticket the caller names is.
class WebGLVkCanvas {
public:
    WebGLVkCanvas(render::VulkanContext& context, WebGLVkStream& stream, const ContextAttributes& attributes = {});
    ~WebGLVkCanvas();

    WebGLVkCanvas(const WebGLVkCanvas&) = delete;
    WebGLVkCanvas& operator=(const WebGLVkCanvas&) = delete;

    /// Create color and depth/stencil attachments of the given size. Their
    /// contents are undefined until recordInit() clears them. Images already
    /// there are released (release(afterTicket)).
    bool init(uint32_t width, uint32_t height, uint64_t afterTicket = 0);

    /// Recreate the attachments at a new size (contents cleared by
    /// recordInit()); the old ones are released after `afterTicket`.
    bool resize(uint32_t width, uint32_t height, uint64_t afterTicket = 0);

    /// Record the clear of freshly created attachments into `cmd` (no-op once done).
    void recordInit(VkCommandBuffer cmd);

    /// Record a clear of every attachment to GL's defaults (transparent
    /// black, or opaque without alpha; depth 1; stencil 0), after whatever
    /// last used them, outside a render pass.
    void recordClear(VkCommandBuffer cmd);

    /// No alpha channel: alpha reads, and is kept, as one.
    bool opaque() const { return !attributes_.alpha; }

    /// Release the images; they are destroyed once the stream's work so far
    /// and queue ticket `afterTicket` (the presenter's last use) are done.
    void release(uint64_t afterTicket);

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

    /// Record the copy of the drawing buffer's color into the front, ordered
    /// after whatever wrote it and before whatever samples the front next
    /// (the presenter's submissions), and after any earlier sampling of it.
    void recordPresentCopy(VkCommandBuffer cmd);
    /// The front, as the presenter samples it (valid once a
    /// recordPresentCopy has been submitted; empty without images).
    render::LayerImage frontImage() const;

    /// Record a copy of the color attachment into `dst` as tightly packed,
    /// top-down RGBA (width x height x 4), leaving the layout as it was.
    bool recordCopy(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize dstOffset = 0);

    // Accessors
    VkImage colorImage() const { return colorImage_; }
    VkImageView colorView() const { return colorView_; }
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
    struct ColorImage {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint64_t allocId = 0;
        VkImageView view = VK_NULL_HANDLE;
        VkImageView presentView = VK_NULL_HANDLE;  // alpha read as one; only without alpha
    };
    bool createColorImage(uint32_t width, uint32_t height, VkImageUsageFlags usage, ColorImage& out);
    bool createColorAttachment(uint32_t width, uint32_t height);
    bool createDepthAttachment(uint32_t width, uint32_t height);
    void clearImages(VkCommandBuffer cmd);

    render::VulkanContext& context_;
    WebGLVkStream& stream_;
    ContextAttributes attributes_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    VkImage colorImage_ = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory_ = VK_NULL_HANDLE;
    VkDeviceSize colorOffset_ = 0;
    uint64_t colorAllocId_ = 0;
    VkImageView colorView_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    ColorImage front_;
    VkImageLayout frontLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

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
