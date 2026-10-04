#pragma once

#include "render/vulkan_context.h"
#include "render/vulkan_swapchain.h"

#include <cstdint>
#include <memory>
#include <vector>

class SkSurface;

namespace bro::render {

/// Handles Vulkan presentation for the Skia UI compositor:
/// - In windowed mode: uploads Skia surface/pixels to staging buffer, copies to swapchain image, and presents.
/// - In headless/offscreen mode: renders into an offscreen VkImage and supports pixel readback.
class VulkanPresenter {
public:
    /// Windowed constructor: takes an existing context and swapchain.
    VulkanPresenter(VulkanContext& context, VulkanSwapchain& swapchain);

    /// Headless/offscreen constructor: operates without a window/swapchain.
    explicit VulkanPresenter(VulkanContext& context);

    ~VulkanPresenter();

    VulkanPresenter(const VulkanPresenter&) = delete;
    VulkanPresenter& operator=(const VulkanPresenter&) = delete;

    /// Initialize command buffers and presentation resources.
    bool init();

    /// Present a Skia surface to the swapchain or offscreen image.
    bool presentSurface(SkSurface* surface);

    /// Present raw 32-bit RGBA/BGRA pixels to the swapchain or offscreen image.
    /// stride is row byte count (0 = width * 4).
    bool presentPixels(const void* pixels, uint32_t width, uint32_t height,
                       uint32_t stride = 0, bool isBgra = false);

    /// Offscreen headless readback: captures pixels from the offscreen VkImage into host memory.
    bool readbackPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

    /// Present an existing GPU VkImage directly without CPU roundtrips (zero-copy UI compositing).
    /// If overlaySurface is non-null, it is alpha-blended over the image on GPU before presentation.
    bool presentImage(VkImage image, uint32_t width, uint32_t height,
                      VkImageLayout currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      SkSurface* overlaySurface = nullptr);

    bool isHeadless() const { return swapchain_ == nullptr; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    VkImage offscreenImage() const { return offscreenImage_; }
    VkImageView offscreenView() const { return offscreenView_; }

private:
    bool ensureStagingBuffer(VkDeviceSize requiredSize);
    bool ensureOffscreenImage(uint32_t width, uint32_t height);
    bool ensureOverlayImage(uint32_t width, uint32_t height);
    bool uploadOverlaySurface(SkSurface* surface, VkCommandBuffer cmd);
    bool initOverlayPipeline(VkFormat targetFormat, VkRenderPass& outRenderPass, VkPipeline& outPipeline);
    bool recordOverlayPass(VkCommandBuffer cmd, VkImage targetImage, VkImageView targetView,
                           VkRenderPass renderPass, VkPipeline pipeline, VkFramebuffer& framebuffer,
                           uint32_t targetWidth, uint32_t targetHeight, VkFormat targetFormat);
    void cleanupOverlay();
    void cleanup();

    VulkanContext& context_;
    VulkanSwapchain* swapchain_ = nullptr; // null in headless mode

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Staging buffer for uploading CPU pixels to GPU
    VkBuffer stagingBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    VkDeviceSize stagingBufferSize_ = 0;

    // Windowed mode command buffers (one per flight frame)
    std::vector<VkCommandBuffer> commandBuffers_;

    // Offscreen mode resources
    VkImage offscreenImage_ = VK_NULL_HANDLE;
    VkDeviceMemory offscreenMemory_ = VK_NULL_HANDLE;
    VkImageView offscreenView_ = VK_NULL_HANDLE;

    // Readback buffer for headless captures
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkDeviceSize readbackBufferSize_ = 0;

    // Overlay compositing resources
    VkImage overlayImage_ = VK_NULL_HANDLE;
    VkDeviceMemory overlayMemory_ = VK_NULL_HANDLE;
    VkImageView overlayView_ = VK_NULL_HANDLE;
    uint32_t overlayW_ = 0;
    uint32_t overlayH_ = 0;

    VkSampler overlaySampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout overlayDescriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool overlayDescriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet overlayDescriptorSet_ = VK_NULL_HANDLE;
    VkPipelineLayout overlayPipelineLayout_ = VK_NULL_HANDLE;

    VkRenderPass overlayRenderPassOffscreen_ = VK_NULL_HANDLE;
    VkPipeline overlayPipelineOffscreen_ = VK_NULL_HANDLE;
    VkFramebuffer overlayFramebufferOffscreen_ = VK_NULL_HANDLE;

    VkRenderPass overlayRenderPassSwapchain_ = VK_NULL_HANDLE;
    VkPipeline overlayPipelineSwapchain_ = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> overlayFramebuffersSwapchain_;
};

} // namespace bro::render
