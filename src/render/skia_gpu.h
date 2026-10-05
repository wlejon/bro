#pragma once

// Skia on the GPU: one Ganesh-Vulkan context on bro's VulkanContext, shared by
// every thread that draws with Skia (the raster thread's UI layers, the main
// thread's 2D canvases, headless captures), and the surfaces it draws into.
//
// Sharing. Skia gets the engine's device and its one graphics queue. Its
// vkQueueSubmit / vkQueueWaitIdle / vkDeviceWaitIdle are routed through the
// context's VulkanQueue (VulkanQueue::submitForeign, queue-scoped waits), so
// Skia's work takes tickets like every other submission and the single-queue-
// owner rule holds.
//
// Threads. A GrDirectContext is not thread-safe, so every call that reaches it
// — drawing on a GPU surface, flushing, creating a surface, dropping one, a
// GPU readback — runs under lock(). The lock is recursive (a canvas flushed
// inline while the raster thread replays a panel nests it). Nothing may wait
// for another thread while holding it.
//
// Images. A GPU LayerSurface draws into a VkImage bro allocates (SkiaImage),
// so the presenter samples it in place, without a readback or a copy. At every
// submission boundary such an image is in SHADER_READ_ONLY_OPTIMAL: surfaces
// are left there by finish(), which every producer calls before handing an
// image on. A SkiaImage outlives whoever still holds it — the surface, Skia's
// in-flight work, a compositor's layer list — and is destroyed once the queue
// has finished every submission made before the last reference went.

#include "render/vulkan_context.h"

#include <include/core/SkRefCnt.h>
#include <include/core/SkSurface.h>

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

class GrDirectContext;

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

class SkiaGpu {
public:
    explicit SkiaGpu(VulkanContext& vulkan);
    ~SkiaGpu();

    SkiaGpu(const SkiaGpu&) = delete;
    SkiaGpu& operator=(const SkiaGpu&) = delete;

    /// Create the Skia context on the device. False (logged) when Skia cannot
    /// run on it; the engine then draws with the CPU.
    bool init();

    VulkanContext& vulkan() { return vulkan_; }
    GrDirectContext* context() const { return context_.get(); }

    /// Held by whoever touches the Skia context. Recursive.
    class Lock {
    public:
        Lock() = default;
        explicit Lock(SkiaGpu& gpu);
        Lock(Lock&& other) noexcept;
        Lock& operator=(Lock&& other) noexcept;
        ~Lock();
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        SkiaGpu* gpu_ = nullptr;
    };
    [[nodiscard]] Lock lock() { return Lock(*this); }
    /// Whether the calling thread holds the lock (for assertions).
    bool heldByThisThread() const { return owner_.load(std::memory_order_relaxed) == std::this_thread::get_id(); }

    /// A width x height RGBA8 premultiplied GPU surface, cleared to
    /// transparent and finished. Empty on failure. Takes the lock.
    LayerSurface makeSurface(int width, int height);

    /// Flush every pending draw, leave `surfaces`' images ready to sample
    /// and submit. Takes the lock.
    void finish(std::span<SkSurface* const> surfaces);
    void finish(SkSurface* surface) { finish(std::span<SkSurface* const>(&surface, 1)); }

    /// Destroy the images whose last use the GPU has finished. Any thread;
    /// does not take the lock.
    void collect();

private:
    friend struct SkiaImage;
    friend class Lock;
    void retire(VkImage image, VkImageView view, uint64_t allocId);

    VulkanContext& vulkan_;
    sk_sp<GrDirectContext> context_;

    std::recursive_mutex mutex_;
    std::atomic<std::thread::id> owner_{};
    int depth_ = 0;  // guarded by mutex_

    struct Retired {
        VkImage image;
        VkImageView view;
        uint64_t allocId;
        uint64_t ticket;
    };
    std::mutex retireMutex_;
    std::vector<Retired> retired_;
    std::atomic<int> liveImages_{0};
};

} // namespace bro::render
