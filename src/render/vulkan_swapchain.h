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
    Error,
};

/// Surface capabilities, supported surface formats, and present modes.
struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

/// Vulkan swapchain management: surface creation, image views, double/triple buffering, semaphores and fences.
class VulkanSwapchain {
public:
    static constexpr size_t kMaxFramesInFlight = 2; // double buffering for frame pacing

    VulkanSwapchain(VulkanContext& context, SDL_Window* window, bool vsync = true);
    ~VulkanSwapchain();

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    /// Initialize the surface, swapchain, image views, and sync objects.
    bool init();

    /// Recreate swapchain on window resize or when suboptimal/out-of-date.
    bool resize(uint32_t width, uint32_t height);

    /// Change vsync mode and recreate swapchain if needed.
    void setVSync(bool vsync);
    bool vsync() const { return vsync_; }

    /// Acquire the next available image from the swapchain for the current in-flight frame.
    /// Waits on the in-flight fence and signals imageAvailableSemaphore.
    SwapchainResult acquireNextImage(uint32_t& outImageIndex, uint64_t timeoutNs = UINT64_MAX);

    /// Present the given swapchain image index after rendering/transfer completes.
    /// Waits on renderFinishedSemaphore and submits to the present queue.
    SwapchainResult present(uint32_t imageIndex);

    VkSurfaceKHR surface() const { return surface_; }
    VkSwapchainKHR swapchain() const { return swapchain_; }
    VkExtent2D extent() const { return extent_; }
    VkFormat imageFormat() const { return imageFormat_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }

    VkImage image(uint32_t index) const { return images_[index]; }
    VkImageView imageView(uint32_t index) const { return imageViews_[index]; }

    size_t currentFrame() const { return currentFrame_; }
    VkSemaphore currentImageAvailableSemaphore() const { return imageAvailableSemaphores_[currentFrame_]; }
    VkSemaphore currentRenderFinishedSemaphore() const { return renderFinishedSemaphores_[currentFrame_]; }
    VkFence currentInFlightFence() const { return inFlightFences_[currentFrame_]; }

    static SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface);

private:
    bool createSurface();
    bool createSwapchain(uint32_t width, uint32_t height);
    bool createImageViews();
    bool createSyncObjects();
    void cleanupSwapchain();
    void cleanup();

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height);

    VulkanContext& context_;
    SDL_Window* window_ = nullptr;
    bool vsync_ = true;

    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat imageFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};

    std::vector<VkImage> images_;
    std::vector<VkImageView> imageViews_;

    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    std::vector<VkFence> inFlightFences_;

    size_t currentFrame_ = 0;
};

} // namespace bro::render
