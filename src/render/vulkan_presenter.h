#pragma once

#include "render/vulkan_context.h"
#include "render/vulkan_swapchain.h"

#include <cstdint>
#include <map>
#include <vector>

class SkSurface;

namespace bro::render {

/// A CPU layer of a PresentFrame: 32-bit premultiplied pixels.
struct PresentPixels {
    const void* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;   // row bytes; 0 = width * 4
    bool bgra = false;     // byte order
    explicit operator bool() const { return pixels && width > 0 && height > 0; }
};

/// One frame for VulkanPresenter::present, bottom to top: CPU pixels below
/// (the UI under a 3D scene, or the whole frame), an optional GPU image (a
/// full-viewport scene or WebGL canvas), CPU pixels above (the UI over it).
/// Every layer sits 1:1 at the target's top-left; the image and the layer
/// above blend with premultiplied alpha. Whatever no layer covers is
/// `clearColor`.
struct PresentFrame {
    PresentPixels below;

    VkImage image = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // restored afterwards
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;

    PresentPixels above;

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    bool hasImage() const { return image != VK_NULL_HANDLE && imageWidth > 0 && imageHeight > 0; }
};

/// Puts finished frames on screen (windowed: a VulkanSwapchain) or into an
/// offscreen image that can be read back (headless).
///
/// Every frame is one command buffer from the context's VulkanFrames ring and
/// one submission through its VulkanQueue: per-frame staging comes from the
/// ring's upload arena, the textures the blended layers are drawn from are
/// per frame slot, and their descriptor sets are fresh per-frame allocations,
/// so nothing the GPU may still be reading is ever rewritten. Resized
/// resources are retired through the ring's deferred destruction, never a
/// device wait.
class VulkanPresenter {
public:
    /// Windowed: present to `swapchain`.
    VulkanPresenter(VulkanContext& context, VulkanSwapchain& swapchain);
    /// Offscreen: present into an image readable with readbackPixels().
    explicit VulkanPresenter(VulkanContext& context);

    ~VulkanPresenter();

    VulkanPresenter(const VulkanPresenter&) = delete;
    VulkanPresenter& operator=(const VulkanPresenter&) = delete;

    bool init();

    /// Present one frame. Windowed, a minimized window presents nothing and
    /// returns true. Offscreen, the frame is also copied into the readback
    /// buffer in the same submission.
    bool present(const PresentFrame& frame);

    /// Convenience wrappers over present(): a surface or pixels as the whole
    /// frame, or an image with an optional surface over it.
    bool presentSurface(SkSurface* surface);
    bool presentPixels(const void* pixels, uint32_t width, uint32_t height,
                       uint32_t stride = 0, bool isBgra = false);
    bool presentImage(VkImage image, uint32_t width, uint32_t height,
                      VkImageLayout currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      SkSurface* overlaySurface = nullptr);

    /// The pixels of an 8-bit RGBA/BGRA surface as a PresentPixels layer
    /// (empty for any other kind of surface).
    static PresentPixels surfaceLayer(SkSurface* surface);

    /// Offscreen: wait for the last present and return its pixels (RGBA8,
    /// tightly packed). False if nothing has been presented since the last
    /// readback-invalidating resize, or in windowed mode.
    bool readbackPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

    bool isHeadless() const { return swapchain_ == nullptr; }
    VulkanSwapchain* swapchain() const { return swapchain_; }

    /// Size of the last presented frame (the swapchain extent when windowed).
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    VkImage offscreenImage() const { return offscreen_.image; }
    VkImageView offscreenView() const { return offscreen_.view; }

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        uint64_t allocId = 0;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t lastUseSerial = 0;
    };
    struct Target {
        VkImage image;
        VkImageView view;
        VkFormat format;
        uint32_t width;
        uint32_t height;
    };
    // A texture blended onto the target, 1:1 at its top-left.
    struct BlendDraw {
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    // vulkan_presenter.cpp
    bool presentToSwapchain(const PresentFrame& frame);
    bool presentOffscreen(const PresentFrame& frame);
    bool recordFrame(VkCommandBuffer cmd, const PresentFrame& frame, const Target& target,
                     VkPipelineStageFlags acquireStages, VkImageLayout& targetLayout);
    bool ensureImage(Image& img, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
    void retireImage(Image& img);
    void destroyImageNow(Image& img);
    bool ensureReadbackBuffer(VkDeviceSize size);
    void cleanup();

    // vulkan_presenter_blend.cpp
    bool initBlendResources();
    void destroyBlendResources();
    VkPipeline blendPipeline(VkFormat targetFormat);
    Image* slotTexture(Image (&ring)[VulkanFrames::kFramesInFlight], uint32_t width, uint32_t height,
                       VkFormat format);
    bool uploadLayerTexture(VkCommandBuffer cmd, const PresentPixels& layer, BlendDraw& out);
    bool copyImageTexture(VkCommandBuffer cmd, const PresentFrame& frame, BlendDraw& out);
    bool describeTexture(const Image& tex, BlendDraw& out);
    void recordBlendDraws(VkCommandBuffer cmd, const Target& target, const BlendDraw* draws, size_t count);

    VulkanContext& context_;
    VulkanSwapchain* swapchain_ = nullptr;  // null offscreen

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Offscreen target and its readback.
    Image offscreen_;
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkDeviceSize readbackOffset_ = 0;
    uint64_t readbackAllocId_ = 0;
    void* readbackMapped_ = nullptr;
    VkDeviceSize readbackSize_ = 0;
    uint64_t readbackTicket_ = 0;  // 0 = nothing presented to read back

    // Blended layers: per frame slot, the layer above's texture and the
    // GPU image's copy (when there is a layer below it to blend onto).
    Image aboveTex_[VulkanFrames::kFramesInFlight];
    Image imageTex_[VulkanFrames::kFramesInFlight];
    VkSampler blendSampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout blendSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blendPipelineLayout_ = VK_NULL_HANDLE;
    std::map<VkFormat, VkPipeline> blendPipelines_;
};

} // namespace bro::render
