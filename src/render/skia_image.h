#pragma once

// The GPU image a Skia surface draws into, and the surface (skia_gpu.h makes
// both). Apart from SkiaGpu so that a header holding a layer's image or
// surface does not take Vulkan's context, the frame ring and Ganesh with it.

#include <include/core/SkRefCnt.h>
#include <include/core/SkSurface.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>

namespace bro::render {

class SkiaGpu;

/// The VkImage under a GPU surface. Its layout, wherever another component
/// reads it, is VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL.
struct SkiaImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;

    SkiaImage() = default;
    SkiaImage(const SkiaImage&) = delete;
    SkiaImage& operator=(const SkiaImage&) = delete;
    ~SkiaImage();

private:
    friend class SkiaGpu;
    friend struct LayerSurface;
    SkiaGpu* gpu_ = nullptr;
    uint64_t allocId_ = 0;
};

using SkiaImageRef = std::shared_ptr<const SkiaImage>;

/// A surface Skia draws into: CPU raster, or (with `image`) GPU. Move-only;
/// releasing a GPU one takes the GPU lock, so it may happen on any thread.
struct LayerSurface {
    sk_sp<SkSurface> surface;
    SkiaImageRef image;  // GPU surfaces only

    LayerSurface() = default;
    LayerSurface(sk_sp<SkSurface> s, SkiaImageRef img) : surface(std::move(s)), image(std::move(img)) {}
    LayerSurface(LayerSurface&& other) noexcept = default;
    LayerSurface& operator=(LayerSurface&& other) noexcept {
        if (this != &other) {
            reset();
            surface = std::move(other.surface);
            image = std::move(other.image);
        }
        return *this;
    }
    LayerSurface(const LayerSurface&) = delete;
    LayerSurface& operator=(const LayerSurface&) = delete;
    ~LayerSurface() { reset(); }

    void reset();
    bool isGpu() const { return image != nullptr; }
    explicit operator bool() const { return surface != nullptr; }
};

}  // namespace bro::render
