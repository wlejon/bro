#pragma once

#include "render/vulkan_context.h"
#include "render/vulkan_swapchain.h"

#include <cstdint>
#include <map>
#include <vector>

class SkSurface;

namespace bro::render {

class VulkanDmabufImporter;
class KmsDirectPresenter;

/// A CPU layer of a PresentFrame: 32-bit premultiplied pixels.
struct PresentPixels {
    const void* pixels = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;   // row bytes; 0 = width * 4
    bool bgra = false;     // byte order
    explicit operator bool() const { return pixels && width > 0 && height > 0; }
};

/// A GPU image of a PresentFrame (a 3D scene's output, a WebGL canvas, a
/// Skia layer), drawn into `dst` in target pixels — scaled when the sizes
/// differ — and cut to `clip`; then `above`, the CPU layer of everything
/// composited after it, at the target's top-left.
///
/// An image with a `view` (sampled usage) is sampled in place — moved to
/// SHADER_READ_ONLY_OPTIMAL for the frame when `layout` is another, and back
/// after; any other (8-bit RGBA/BGRA) is first copied into a texture of the
/// presenter's.
struct PresentImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;  // restored afterwards
    uint32_t width = 0;
    uint32_t height = 0;
    float dstX = 0.0f, dstY = 0.0f, dstW = 0.0f, dstH = 0.0f;
    bool clipped = false;
    VkRect2D clip{};  // when clipped

    PresentPixels above;

    /// `image` 1:1 at the target's top-left.
    static PresentImage at1to1(VkImage image, VkImageLayout layout, uint32_t width, uint32_t height) {
        PresentImage out;
        out.image = image;
        out.layout = layout;
        out.width = width;
        out.height = height;
        out.dstW = static_cast<float>(width);
        out.dstH = static_cast<float>(height);
        return out;
    }
    explicit operator bool() const { return image != VK_NULL_HANDLE && width > 0 && height > 0; }
};

/// One frame for VulkanPresenter::present, bottom to top: CPU pixels below
/// (the UI under the first GPU image, or the whole frame), then each GPU
/// image with the CPU layer above it. CPU layers sit 1:1 at the target's
/// top-left; everything over `below` blends with premultiplied alpha.
/// Whatever no layer covers is `clearColor`.
struct PresentFrame {
    PresentPixels below;
    std::vector<PresentImage> images;

    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    /// Offscreen: the target size. 0 = what the layers cover.
    uint32_t width = 0;
    uint32_t height = 0;

    bool hasImages() const { return !images.empty(); }
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

    /// Windowed: also copy each presented swapchain image into the readback
    /// buffer, in the frame's own submission, so readbackPixels() returns
    /// what was put on screen (where the surface lets swapchain images be
    /// read). Test harnesses only: it costs a copy per frame.
    void setCapturePresents(bool capture) { capturePresents_ = capture; }

    /// Wait for the last present's readback and return its pixels (RGBA8,
    /// tightly packed). Offscreen every present is read back; windowed only
    /// those captured (setCapturePresents). False if there is none.
    bool readbackPixels(std::vector<uint8_t>& outPixels, uint32_t& outWidth, uint32_t& outHeight);

    bool isHeadless() const { return swapchain_ == nullptr; }
    VulkanSwapchain* swapchain() const { return swapchain_; }

    /// Size of the last presented frame (the swapchain extent when windowed).
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    VkImage offscreenImage() const { return offscreen_.image; }
    VkImageView offscreenView() const { return offscreen_.view; }

    /// DMA-BUF client buffer importer and KMS direct presenter
    VulkanDmabufImporter* dmabufImporter() { return dmabufImporter_.get(); }
    const VulkanDmabufImporter* dmabufImporter() const { return dmabufImporter_.get(); }
    KmsDirectPresenter* kmsDirectPresenter() { return kmsPresenter_.get(); }
    const KmsDirectPresenter* kmsDirectPresenter() const { return kmsPresenter_.get(); }

    /// Enable direct KMS scanout if a DRM master fd is available
    bool enableKmsScanout(int drmFd);

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
    // A texture blended onto the target: drawn into `dst` (target pixels),
    // cut to `scissor`.
    struct BlendDraw {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkViewport dst{};
        VkRect2D scissor{};
    };

    // vulkan_presenter.cpp
    bool presentToSwapchain(const PresentFrame& frame);
    bool presentOffscreen(const PresentFrame& frame);
    bool recordFrame(VkCommandBuffer cmd, const PresentFrame& frame, const Target& target,
                     VkPipelineStageFlags acquireStages, VkImageLayout& targetLayout);
    bool recordLayers(VkCommandBuffer cmd, const PresentFrame& frame, const Target& target,
                      VkImageLayout& targetLayout);
    bool ensureImage(Image& img, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage);
    void retireImage(Image& img);
    void destroyImageNow(Image& img);
    bool ensureReadbackBuffer(VkDeviceSize size);
    bool recordReadback(VkCommandBuffer cmd, const Target& target, VkImageLayout& layout);
    void cleanup();

    // vulkan_presenter_blend.cpp
    bool initBlendResources();
    void destroyBlendResources();
    VkPipeline blendPipeline(VkFormat targetFormat);
    using TextureRing = std::vector<Image>[VulkanFrames::kFramesInFlight];
    Image* slotTexture(TextureRing& ring, size_t index, uint32_t width, uint32_t height, VkFormat format);
    bool uploadLayerTexture(VkCommandBuffer cmd, const PresentPixels& layer, size_t index, const Target& target,
                            BlendDraw& out);
    bool copyImageTexture(VkCommandBuffer cmd, const PresentImage& image, size_t index, const Target& target,
                          BlendDraw& out);
    bool describeTexture(VkImageView view, VkSampler sampler, BlendDraw& out);
    bool describeInPlace(const PresentImage& image, const Target& target, BlendDraw& out);
    void recordBlendDraws(VkCommandBuffer cmd, const Target& target, const std::vector<BlendDraw>& draws);

    VulkanContext& context_;
    VulkanSwapchain* swapchain_ = nullptr;  // null offscreen

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    // Offscreen target, and the readback of the last (captured) present.
    Image offscreen_;
    VkBuffer readbackBuffer_ = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory_ = VK_NULL_HANDLE;
    VkDeviceSize readbackOffset_ = 0;
    uint64_t readbackAllocId_ = 0;
    void* readbackMapped_ = nullptr;
    VkDeviceSize readbackSize_ = 0;
    uint64_t readbackTicket_ = 0;  // 0 = nothing presented to read back
    uint32_t readbackW_ = 0;
    uint32_t readbackH_ = 0;
    bool readbackBgra_ = false;     // the read-back target's byte order
    bool capturePresents_ = false;

    // Blended layers: per frame slot, the CPU layers' textures and the GPU
    // images' copies, by their place in the frame.
    TextureRing aboveTex_;
    TextureRing imageTex_;
    VkSampler blendSampler_ = VK_NULL_HANDLE;        // 1:1 draws
    VkSampler blendSamplerLinear_ = VK_NULL_HANDLE;  // scaled images
    VkDescriptorSetLayout blendSetLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout blendPipelineLayout_ = VK_NULL_HANDLE;
    std::map<VkFormat, VkPipeline> blendPipelines_;

    std::unique_ptr<VulkanDmabufImporter> dmabufImporter_;
    std::unique_ptr<KmsDirectPresenter> kmsPresenter_;
};

} // namespace bro::render
