#include "render/vulkan_swapchain.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <limits>

namespace bro::render {

namespace {

// Enough acquire semaphores that, with frames in flight bounding how far the
// CPU runs ahead, picking the next one never has to wait in practice.
constexpr size_t kAcquireSemaphores = VulkanFrames::kFramesInFlight + 2;

VkSemaphore createBinarySemaphore(VkDevice device) {
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore sem = VK_NULL_HANDLE;
    if (vkCreateSemaphore(device, &info, nullptr, &sem) != VK_SUCCESS) return VK_NULL_HANDLE;
    return sem;
}

} // namespace

VulkanSwapchain::VulkanSwapchain(VulkanContext& context, SDL_Window* window, bool vsync)
    : context_(context), window_(window), wantVsync_(vsync), vsync_(vsync)
{
}

VulkanSwapchain::~VulkanSwapchain() {
    cleanup();
}

bool VulkanSwapchain::init() {
    if (!createSurface()) {
        LOG_ERROR("VulkanSwapchain: Failed to create window surface");
        return false;
    }
    if (!context_.canPresentTo(surface_)) {
        LOG_ERROR("VulkanSwapchain: the device's present queue family cannot present to this window");
        cleanup();
        return false;
    }

    acquireSlots_.resize(kAcquireSemaphores);
    for (auto& slot : acquireSlots_) {
        slot.semaphore = createBinarySemaphore(context_.device());
        if (slot.semaphore == VK_NULL_HANDLE) {
            LOG_ERROR("VulkanSwapchain: Failed to create acquire semaphores");
            cleanup();
            return false;
        }
    }

    if (!recreate()) {
        LOG_ERROR("VulkanSwapchain: Failed to create swapchain");
        cleanup();
        return false;
    }

    LOG_INFO("VulkanSwapchain: Initialized (%ux%u, format %d, %zu images, vsync=%s)",
             extent_.width, extent_.height, imageFormat_, images_.size(), vsync_ ? "on" : "off");
    return true;
}

// Hand the current swapchain's views and present semaphores to the frame ring
// to destroy once the GPU has finished the frames that used them; the
// swapchain itself goes the same way (recreate() passes it as oldSwapchain
// first). Presentation of an old image is ordered before the frame that
// retires it completes, because later submissions on the queue wait on it.
void VulkanSwapchain::retireSwapchainObjects() {
    VkDevice device = context_.device();
    std::vector<VkImageView> views = std::move(imageViews_);
    std::vector<VkSemaphore> sems = std::move(presentSemaphores_);
    VkSwapchainKHR old = swapchain_;
    imageViews_.clear();
    presentSemaphores_.clear();
    images_.clear();
    swapchain_ = VK_NULL_HANDLE;
    context_.frames().defer([device, views = std::move(views), sems = std::move(sems), old]() {
        for (VkImageView v : views) vkDestroyImageView(device, v, nullptr);
        for (VkSemaphore s : sems) vkDestroySemaphore(device, s, nullptr);
        if (old != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, old, nullptr);
    });
}

void VulkanSwapchain::cleanup() {
    VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) {
        context_.queue().waitIdle();
        for (VkImageView v : imageViews_) vkDestroyImageView(device, v, nullptr);
        for (VkSemaphore s : presentSemaphores_) vkDestroySemaphore(device, s, nullptr);
        for (auto& slot : acquireSlots_)
            if (slot.semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, slot.semaphore, nullptr);
        if (swapchain_ != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    }
    imageViews_.clear();
    presentSemaphores_.clear();
    acquireSlots_.clear();
    images_.clear();
    swapchain_ = VK_NULL_HANDLE;

    if (surface_ != VK_NULL_HANDLE && context_.instance() != VK_NULL_HANDLE) {
        SDL_Vulkan_DestroySurface(context_.instance(), surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

bool VulkanSwapchain::createSurface() {
    if (!window_ || context_.instance() == VK_NULL_HANDLE) return false;
    if (!SDL_Vulkan_CreateSurface(window_, context_.instance(), nullptr, &surface_)) {
        LOG_ERROR("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanSwapchain::windowPixelSize(uint32_t& w, uint32_t& h) const {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph);
    // A hidden or minimized window shows nothing; presenting to it can block.
    const bool unseen = (SDL_GetWindowFlags(window_) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
    w = static_cast<uint32_t>(std::max(pw, 0));
    h = static_cast<uint32_t>(std::max(ph, 0));
    return !unseen && w > 0 && h > 0;
}

SwapchainSupportDetails VulkanSwapchain::querySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
    }

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
    }

    return details;
}

VkSurfaceFormatKHR VulkanSwapchain::chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
    // UNORM, not SRGB: the frames bro presents are already sRGB-encoded.
    for (const auto& availableFormat : availableFormats) {
        if ((availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM ||
             availableFormat.format == VK_FORMAT_R8G8B8A8_UNORM) &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

VkPresentModeKHR VulkanSwapchain::chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
    if (!wantVsync_) {
        for (const auto& mode : availablePresentModes) {
            if (mode == VK_PRESENT_MODE_MAILBOX_KHR) return mode;
        }
        for (const auto& mode : availablePresentModes) {
            if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR; // guaranteed by Vulkan specification
}

VkExtent2D VulkanSwapchain::chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height) {
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    VkExtent2D actualExtent = {width, height};
    actualExtent.width = std::clamp(actualExtent.width,
                                    capabilities.minImageExtent.width,
                                    capabilities.maxImageExtent.width);
    actualExtent.height = std::clamp(actualExtent.height,
                                     capabilities.minImageExtent.height,
                                     capabilities.maxImageExtent.height);
    return actualExtent;
}

bool VulkanSwapchain::recreate() {
    uint32_t winW = 0, winH = 0;
    windowPixelSize(winW, winH);

    SwapchainSupportDetails support = querySwapchainSupport(context_.physicalDevice(), surface_);
    if (support.formats.empty()) {
        LOG_ERROR("VulkanSwapchain: the surface reports no formats");
        return false;
    }
    const VkSurfaceCapabilitiesKHR& caps = support.capabilities;
    VkExtent2D extent = chooseSwapExtent(caps, winW, winH);
    if (extent.width == 0 || extent.height == 0) return false;  // minimized: try again later
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        LOG_ERROR("VulkanSwapchain: the surface does not support TRANSFER_DST swapchain images");
        return false;
    }

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(support.formats);
    VkPresentModeKHR presentMode = chooseSwapPresentMode(support.presentModes);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & compositeAlpha)) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if (caps.supportedCompositeAlpha & a) { compositeAlpha = a; break; }
        }
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Readable where the surface allows it, so a presented frame can be
    // captured (VulkanPresenter::setCapturePresents).
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
        createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    const auto& indices = context_.queueFamilies();
    uint32_t queueFamilyIndices[] = {
        static_cast<uint32_t>(indices.graphicsFamily),
        static_cast<uint32_t>(indices.presentFamily)
    };
    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = caps.currentTransform;
    createInfo.compositeAlpha = compositeAlpha;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = swapchain_;

    VkDevice device = context_.device();
    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    VkResult res = vkCreateSwapchainKHR(device, &createInfo, nullptr, &newSwapchain);
    if (res != VK_SUCCESS) {
        LOG_ERROR("VulkanSwapchain: vkCreateSwapchainKHR failed (%d)", res);
        return false;
    }
    if (swapchain_ != VK_NULL_HANDLE) retireSwapchainObjects();

    swapchain_ = newSwapchain;
    imageFormat_ = surfaceFormat.format;
    readable_ = (createInfo.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    presentMode_ = presentMode;
    extent_ = extent;
    vsync_ = wantVsync_;
    needsRecreate_ = false;

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(device, swapchain_, &actualImageCount, nullptr);
    images_.resize(actualImageCount);
    vkGetSwapchainImagesKHR(device, swapchain_, &actualImageCount, images_.data());

    imageViews_.assign(images_.size(), VK_NULL_HANDLE);
    presentSemaphores_.assign(images_.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images_[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageFormat_;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &viewInfo, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            LOG_ERROR("VulkanSwapchain: Failed to create swapchain image views");
            return false;
        }
        presentSemaphores_[i] = createBinarySemaphore(device);
        if (presentSemaphores_[i] == VK_NULL_HANDLE) {
            LOG_ERROR("VulkanSwapchain: Failed to create present semaphores");
            return false;
        }
    }
    return true;
}

SwapchainResult VulkanSwapchain::acquire(uint32_t& outImageIndex) {
    uint32_t winW = 0, winH = 0;
    if (!windowPixelSize(winW, winH)) return SwapchainResult::Minimized;
    if (swapchain_ == VK_NULL_HANDLE || needsRecreate_ || vsync_ != wantVsync_ ||
        winW != extent_.width || winH != extent_.height) {
        if (!recreate()) {
            // A zero extent from the surface is a minimize race, not an error.
            return swapchain_ == VK_NULL_HANDLE ? SwapchainResult::Error : SwapchainResult::Minimized;
        }
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        acquireCursor_ = (acquireCursor_ + 1) % acquireSlots_.size();
        AcquireSlot& slot = acquireSlots_[acquireCursor_];
        context_.queue().wait(slot.ticket);

        VkResult result = vkAcquireNextImageKHR(context_.device(), swapchain_, UINT64_MAX,
                                                slot.semaphore, VK_NULL_HANDLE, &outImageIndex);
        if (result == VK_SUCCESS) return SwapchainResult::Success;
        if (result == VK_SUBOPTIMAL_KHR) {
            needsRecreate_ = true;  // usable now; rebuilt at the next acquire
            return SwapchainResult::Suboptimal;
        }
        if (result != VK_ERROR_OUT_OF_DATE_KHR) {
            LOG_ERROR("VulkanSwapchain: vkAcquireNextImageKHR returned %d", result);
            return SwapchainResult::Error;
        }
        if (!recreate()) return SwapchainResult::Minimized;
    }
    return SwapchainResult::OutOfDate;
}

SwapchainResult VulkanSwapchain::present(uint32_t imageIndex, uint64_t ticket) {
    acquireSlots_[acquireCursor_].ticket = ticket;

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &presentSemaphores_[imageIndex];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = context_.queue().present(presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        needsRecreate_ = true;
        return result == VK_SUBOPTIMAL_KHR ? SwapchainResult::Suboptimal : SwapchainResult::OutOfDate;
    }
    if (result != VK_SUCCESS) {
        LOG_ERROR("VulkanSwapchain: vkQueuePresentKHR returned %d", result);
        return SwapchainResult::Error;
    }
    return SwapchainResult::Success;
}

} // namespace bro::render
