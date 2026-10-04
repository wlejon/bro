#pragma once

#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace bro::webgl::vk {

/// Manages the offscreen canvas render target (VkImage color + depth/stencil attachments),
/// resizing, layout transitions, zero-copy presentation, and staging pixel readback.
class WebGLVkCanvas {
public:
    explicit WebGLVkCanvas(render::VulkanContext& context);
    ~WebGLVkCanvas();

    WebGLVkCanvas(const WebGLVkCanvas&) = delete;
    WebGLVkCanvas& operator=(const WebGLVkCanvas&) = delete;

    /// Initialize color and depth/stencil attachments with given dimensions.
    bool init(uint32_t width, uint32_t height);

    /// Resize color and depth attachments (preserves format, clears previous image content).
    bool resize(uint32_t width, uint32_t height);

    /// Destroy allocated Vulkan image and memory resources.
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

    /// Read canvas color pixels as tightly packed, top-down RGBA (width x height x 4).
    /// Uses Vulkan staging buffer readback without CPU readback during presentation.
    bool readCanvasPixels(std::vector<uint8_t>& out);

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
    bool createColorAttachment(uint32_t width, uint32_t height);
    bool createDepthAttachment(uint32_t width, uint32_t height);
    VkFormat findSupportedDepthFormat();

    render::VulkanContext& context_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;

    VkImage colorImage_ = VK_NULL_HANDLE;
    VkDeviceMemory colorMemory_ = VK_NULL_HANDLE;
    VkDeviceSize colorOffset_ = 0;
    uint64_t colorAllocId_ = 0;
    VkImageView colorView_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageLayout colorLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage depthImage_ = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory_ = VK_NULL_HANDLE;
    VkDeviceSize depthOffset_ = 0;
    uint64_t depthAllocId_ = 0;
    VkImageView depthView_ = VK_NULL_HANDLE;
    VkFormat depthFormat_ = VK_FORMAT_D24_UNORM_S8_UINT;
    VkImageLayout depthLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    // Readback staging resources
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkDeviceSize readbackOffset_ = 0;
    uint64_t readbackAllocId_ = 0;
    void* readbackMapped_ = nullptr;
    VkDeviceSize readbackBufferSize_ = 0;
};

} // namespace bro::webgl::vk
