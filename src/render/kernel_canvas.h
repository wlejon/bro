#pragma once

// What a <canvas> with a 'kernel' context shows (docs/kernel-api.js): an
// image the canvas's KernelBuffers are stored into, in the graphics stream
// after the kernels that wrote them, which the compositor samples as the
// canvas's layer. The GPU-only way to show what a kernel made in the page:
// nothing is read back.

#include "render/layer_image.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bro::render {

class ComputeBuffer;
class VulkanContext;

/// The compute pipelines (shaders/kernel_canvas.bk) that store a buffer's
/// texels into a canvas's image, shared by every KernelCanvas of a context.
class KernelCanvasBlit {
public:
    explicit KernelCanvasBlit(VulkanContext& context);
    ~KernelCanvasBlit();
    KernelCanvasBlit(const KernelCanvasBlit&) = delete;
    KernelCanvasBlit& operator=(const KernelCanvasBlit&) = delete;

    bool valid() const { return pipelines_[0] != VK_NULL_HANDLE && pipelines_[1] != VK_NULL_HANDLE; }
    const std::string& error() const { return error_; }

    /// Record the store of `width` x `height` texels of `buffer` from texel
    /// `offset` into `view` (a storage image in GENERAL layout) into `cmd`.
    bool record(VkCommandBuffer cmd, const ComputeBuffer& buffer, uint32_t offset, VkImageView view, uint32_t width,
                uint32_t height, bool rgba32f);

private:
    VulkanContext& context_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelines_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};   // rgba8, rgba32f
    std::string error_;
};

class KernelCanvas {
public:
    /// A texel of the buffers shown: RGBA8 (one u32 a pixel, red in its low
    /// byte: `r | g << 8 | b << 16 | a << 24`) or RGBA32F (a vec4<f32>).
    /// Colour is premultiplied by alpha, as the compositor blends it.
    enum class Format { RGBA8, RGBA32F };

    KernelCanvas(VulkanContext& context, std::shared_ptr<KernelCanvasBlit> blit);
    ~KernelCanvas();
    KernelCanvas(const KernelCanvas&) = delete;
    KernelCanvas& operator=(const KernelCanvas&) = delete;

    /// Show `width` x `height` texels of `buffer` from byte `offset` (rows
    /// top to bottom, packed tight; a whole number of texels) from the next
    /// composite on. False, with the reason in `*err`, when they run past the
    /// buffer, the offset is not a texel's, or the image cannot be made.
    bool present(const ComputeBuffer& buffer, uint64_t offset, uint32_t width, uint32_t height, Format format,
                 std::string* err);

    /// The image shown (empty before the first present).
    LayerImage image() const;
    /// Bumped by every present.
    uint64_t generation() const { return generation_; }

private:
    bool ensureImage(uint32_t width, uint32_t height, VkFormat format, std::string* err);
    void releaseImage();

    VulkanContext& context_;
    std::shared_ptr<KernelCanvasBlit> blit_;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    uint64_t allocId_ = 0;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkImageLayout layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t generation_ = 0;
};

} // namespace bro::render
