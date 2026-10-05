#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <vector>

struct SDL_Window;

namespace bro::render {

class VulkanContext;

/// Result of swapchain acquire and present operations.
enum class SwapchainResult {
    Success,
    Suboptimal,
    OutOfDate,
    Minimized,  // the window is minimized, hidden or empty: nothing to present this frame
    Error,
};

/// Surface capabilities, supported surface formats, and present modes.
struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

/// The swapchain of one SDL window.
///
/// The swapchain follows the window: acquire() recreates it when the window's
/// pixel size, the surface, or the vsync preference changed, and reports
/// Minimized (presenting nothing) while the window is minimized, hidden or
/// has no pixels. Old
/// swapchains, views and semaphores are retired through the context's frame
/// ring rather than a device wait.
///
/// Synchronisation: each acquire uses a semaphore from a small ring, reused
/// once the submission that waited on it (its ticket, given to present()) has
/// completed; the semaphore a submission signals for presentation belongs to
/// the swapchain image, so it is only reused once that image is acquired again.
/// CPU/GPU frame pacing is VulkanFrames' job, so there are no fences here.
class VulkanSwapchain {
public:
    VulkanSwapchain(VulkanContext& context, SDL_Window* window, bool vsync = true);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    /// Create the surface and the first swapchain.
    bool init();

    /// FIFO when true; MAILBOX (or IMMEDIATE) when false. Applied at the next acquire.
    void setVSync(bool vsync) { wantVsync_ = vsync; }
    bool vsync() const { return wantVsync_; }

    /// Acquire the next image. On Success/Suboptimal, `outImageIndex` is valid
    /// and acquireSemaphore() is the semaphore the acquisition signals; the
    /// caller must submit work that waits on it and then call present().
    SwapchainResult acquire(uint32_t& outImageIndex);
    VkSemaphore acquireSemaphore() const { return acquireSlots_[acquireCursor_].semaphore; }

    /// The semaphore the submission rendering image `imageIndex` signals and
    /// present() waits on.
    VkSemaphore presentSemaphore(uint32_t imageIndex) const { return presentSemaphores_[imageIndex]; }

    /// Present `imageIndex`. `ticket` is the queue ticket of the submission that
    /// waited on acquireSemaphore() and signalled presentSemaphore(imageIndex).
    SwapchainResult present(uint32_t imageIndex, uint64_t ticket);

    VkSurfaceKHR surface() const { return surface_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkExtent2D extent() const { return extent_; }
    VkFormat imageFormat() const { return imageFormat_; }
    /// Whether the images can be copied from (TRANSFER_SRC).
    bool readable() const { return readable_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }
    VkPresentModeKHR presentMode() const { return presentMode_; }

    VkImage image(uint32_t index) const { return images_[index]; }
    VkImageView imageView(uint32_t index) const { return imageViews_[index]; }

    static SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
    struct AcquireSlot {
        VkSemaphore semaphore = VK_NULL_HANDLE;
        uint64_t ticket = 0;  // submission that waited on it; reusable once complete
    };

    bool createSurface();
    bool recreate();
    bool windowPixelSize(uint32_t& w, uint32_t& h) const;
    void retireSwapchainObjects();
    void cleanup();

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);

    VulkanContext& context_;
    SDL_Window* window_ = nullptr;
    bool wantVsync_ = true;
    bool vsync_ = true;          // the mode the current swapchain was made with
    bool needsRecreate_ = false;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    bool readable_ = false;
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent_{};

    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;
    std::vector<VkSemaphore> presentSemaphores_;  // one per image

    std::vector<AcquireSlot> acquireSlots_;
    size_t acquireCursor_ = 0;
};

} // namespace bro::render
