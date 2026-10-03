#include "render/vulkan_swapchain.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <limits>

namespace bro::render {

VulkanSwapchain::VulkanSwapchain(VulkanContext& context, SDL_Window* window, bool vsync)
    : context_(context), window_(window), vsync_(vsync)
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

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    if (w <= 0 || h <= 0) {
        w = 800;
        h = 600;
    }

    if (!createSwapchain(static_cast<uint32_t>(w), static_cast<uint32_t>(h))) {
        LOG_ERROR("VulkanSwapchain: Failed to create swapchain");
        cleanup();
        return false;
    }

    if (!createImageViews()) {
        LOG_ERROR("VulkanSwapchain: Failed to create image views");
        cleanup();
        return false;
    }

    if (!createSyncObjects()) {
        LOG_ERROR("VulkanSwapchain: Failed to create synchronization objects");
        cleanup();
        return false;
    }

    LOG_INFO("VulkanSwapchain: Initialized (%ux%u, format %d, %zu images, vsync=%s)",
             extent_.width, extent_.height, imageFormat_, images_.size(), vsync_ ? "on" : "off");
    return true;
}

void VulkanSwapchain::cleanupSwapchain() {
    VkDevice device = context_.device();
    if (device == VK_NULL_HANDLE) return;

    for (auto imageView : imageViews_) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, imageView, nullptr);
        }
    }
    imageViews_.clear();
    images_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

void VulkanSwapchain::cleanup() {
    VkDevice device = context_.device();
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);

        for (size_t i = 0; i < inFlightFences_.size(); ++i) {
            if (imageAvailableSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, imageAvailableSemaphores_[i], nullptr);
            }
            if (renderFinishedSemaphores_[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, renderFinishedSemaphores_[i], nullptr);
            }
            if (inFlightFences_[i] != VK_NULL_HANDLE) {
                vkDestroyFence(device, inFlightFences_[i], nullptr);
            }
        }
        imageAvailableSemaphores_.clear();
        renderFinishedSemaphores_.clear();
        inFlightFences_.clear();
    }

    cleanupSwapchain();

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
    if (!vsync_) {
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

bool VulkanSwapchain::createSwapchain(uint32_t width, uint32_t height) {
    SwapchainSupportDetails swapchainSupport = querySwapchainSupport(context_.physicalDevice(), surface_);

    VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapchainSupport.formats);
    VkPresentModeKHR presentMode = chooseSwapPresentMode(swapchainSupport.presentModes);
    VkExtent2D extent = chooseSwapExtent(swapchainSupport.capabilities, width, height);

    uint32_t imageCount = swapchainSupport.capabilities.minImageCount + 1;
    if (swapchainSupport.capabilities.maxImageCount > 0 &&
        imageCount > swapchainSupport.capabilities.maxImageCount) {
        imageCount = swapchainSupport.capabilities.maxImageCount;
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

    createInfo.preTransform = swapchainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = swapchain_;

    VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(context_.device(), &createInfo, nullptr, &newSwapchain) != VK_SUCCESS) {
        return false;
    }

    if (swapchain_ != VK_NULL_HANDLE) {
        cleanupSwapchain();
    }
    swapchain_ = newSwapchain;
    imageFormat_ = surfaceFormat.format;
    extent_ = extent;

    uint32_t actualImageCount = 0;
    vkGetSwapchainImagesKHR(context_.device(), swapchain_, &actualImageCount, nullptr);
    images_.resize(actualImageCount);
    vkGetSwapchainImagesKHR(context_.device(), swapchain_, &actualImageCount, images_.data());

    return true;
}

bool VulkanSwapchain::createImageViews() {
    imageViews_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = images_[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = imageFormat_;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(context_.device(), &createInfo, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanSwapchain::createSyncObjects() {
    imageAvailableSemaphores_.resize(kMaxFramesInFlight);
    renderFinishedSemaphores_.resize(kMaxFramesInFlight);
    inFlightFences_.resize(kMaxFramesInFlight);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &imageAvailableSemaphores_[i]) != VK_SUCCESS ||
            vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &renderFinishedSemaphores_[i]) != VK_SUCCESS ||
            vkCreateFence(context_.device(), &fenceInfo, nullptr, &inFlightFences_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool VulkanSwapchain::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return true; // minimized

    vkDeviceWaitIdle(context_.device());

    if (!createSwapchain(width, height)) {
        LOG_ERROR("VulkanSwapchain: Failed to recreate swapchain during resize");
        return false;
    }
    if (!createImageViews()) {
        LOG_ERROR("VulkanSwapchain: Failed to recreate image views during resize");
        return false;
    }
    return true;
}

void VulkanSwapchain::setVSync(bool vsync) {
    if (vsync_ == vsync) return;
    vsync_ = vsync;
    resize(extent_.width, extent_.height);
}

SwapchainResult VulkanSwapchain::acquireNextImage(uint32_t& outImageIndex, uint64_t timeoutNs) {
    VkDevice device = context_.device();

    vkWaitForFences(device, 1, &inFlightFences_[currentFrame_], VK_TRUE, timeoutNs);

    VkResult result = vkAcquireNextImageKHR(device, swapchain_, timeoutNs,
                                            imageAvailableSemaphores_[currentFrame_],
                                            VK_NULL_HANDLE, &outImageIndex);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return SwapchainResult::OutOfDate;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        LOG_ERROR("VulkanSwapchain: vkAcquireNextImageKHR returned %d", result);
        return SwapchainResult::Error;
    }

    vkResetFences(device, 1, &inFlightFences_[currentFrame_]);
    return (result == VK_SUBOPTIMAL_KHR) ? SwapchainResult::Suboptimal : SwapchainResult::Success;
}

SwapchainResult VulkanSwapchain::present(uint32_t imageIndex) {
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinishedSemaphores_[currentFrame_];
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;

    VkResult result = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return SwapchainResult::OutOfDate;
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        return SwapchainResult::Suboptimal;
    }
    if (result != VK_SUCCESS) {
        LOG_ERROR("VulkanSwapchain: vkQueuePresentKHR returned %d", result);
        return SwapchainResult::Error;
    }

    return SwapchainResult::Success;
}

} // namespace bro::render
