#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::render {

/// Queue family indices identified during device selection.
struct VulkanQueueFamilyIndices {
    int graphicsFamily = -1;
    int presentFamily = -1;
    int computeFamily = -1;
    int transferFamily = -1;

    bool isComplete(bool needPresent = true) const {
        return graphicsFamily >= 0 && (!needPresent || presentFamily >= 0);
    }
};

/// Configuration parameters for VulkanContext initialization.
struct VulkanContextConfig {
    bool enableValidation = false;
    bool headless = false;
    int preferredDeviceIndex = -1; // -1 = auto-select best GPU
    std::vector<std::string> extraInstanceExtensions;
    std::vector<std::string> extraDeviceExtensions;
    bool enableDynamicRendering = true;
};

/// Vulkan instance, physical device, logical device, and queue management.
class VulkanContext {
public:
    explicit VulkanContext(const VulkanContextConfig& config = {});
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /// Initialize the Vulkan instance, physical device, logical device, queues, and command pool.
    /// If compatibleSurface is provided (e.g. from an SDL window), present queue support will be verified on it.
    bool init(VkSurfaceKHR compatibleSurface = VK_NULL_HANDLE);

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }

    VkQueue graphicsQueue() const { return graphicsQueue_; }
    VkQueue presentQueue() const { return presentQueue_; }
    VkQueue computeQueue() const { return computeQueue_; }
    VkQueue transferQueue() const { return transferQueue_; }

    const VulkanQueueFamilyIndices& queueFamilies() const { return queueIndices_; }
    const VkPhysicalDeviceProperties& deviceProperties() const { return deviceProperties_; }
    const VkPhysicalDeviceMemoryProperties& memoryProperties() const { return memoryProperties_; }

    VkCommandPool commandPool() const { return commandPool_; }

    /// Single-use command buffer helpers for transfers and layout transitions.
    VkCommandBuffer beginSingleTimeCommands() const;
    void endSingleTimeCommands(VkCommandBuffer commandBuffer) const;

    /// Memory and resource allocation helpers.
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& bufferMemory) const;

    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory) const;

    void copyBufferToImage(VkBuffer buffer, VkImage image,
                           uint32_t width, uint32_t height,
                           VkCommandBuffer cmd = VK_NULL_HANDLE) const;

    void copyImageToBuffer(VkImage image, VkBuffer buffer,
                           uint32_t width, uint32_t height,
                           VkCommandBuffer cmd = VK_NULL_HANDLE) const;

    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               VkCommandBuffer cmd = VK_NULL_HANDLE) const;

    void waitIdle() const;

    bool isHeadless() const { return config_.headless; }
    bool isValid() const { return device_ != VK_NULL_HANDLE; }

    /// Enumerate available queue families for a physical device, testing presentation against `surface` if provided.
    static VulkanQueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface = VK_NULL_HANDLE);

private:
    bool createInstance();
    bool setupDebugMessenger();
    bool selectPhysicalDevice(VkSurfaceKHR compatibleSurface);
    bool createLogicalDevice();
    bool createCommandPool();
    void cleanup();

    VulkanContextConfig config_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;

    VulkanQueueFamilyIndices queueIndices_;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkQueue computeQueue_ = VK_NULL_HANDLE;
    VkQueue transferQueue_ = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties deviceProperties_{};
    VkPhysicalDeviceMemoryProperties memoryProperties_{};
    VkPhysicalDeviceFeatures deviceFeatures_{};

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
};

} // namespace bro::render
