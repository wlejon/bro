#pragma once

#include "render/vulkan_frames.h"
#include "render/vulkan_memory_pool.h"
#include "render/vulkan_queue.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct SDL_Window;

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

/// Vulkan instance, physical device and logical device, plus the two pieces
/// every GPU consumer shares: the VulkanQueue that owns all submission and
/// presentation, and the VulkanFrames ring of frames in flight.
///
/// bro needs Vulkan 1.3 with dynamic rendering and timeline semaphores; a
/// device without them is skipped at selection. synchronization2 is enabled
/// when the device has it (hasSynchronization2()).
class VulkanContext {
public:
    explicit VulkanContext(const VulkanContextConfig& config = {});
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    /// Create the instance, pick a physical device, create the logical device,
    /// the queue owner and the frame ring. With `presentTarget` (windowed), the
    /// chosen device and present queue family must be able to present to a
    /// surface on that window.
    bool init(SDL_Window* presentTarget = nullptr);

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

    /// The effective API version: the lower of what the instance asked for
    /// (1.3) and what the device supports.
    uint32_t apiVersion() const { return apiVersion_; }
    bool hasSynchronization2() const { return synchronization2_; }

    VulkanQueue& queue() { return queue_; }
    VulkanFrames& frames() { return frames_; }

    VkCommandPool commandPool() const { return commandPool_; }

    VulkanMemoryPool& memoryPool() { return memoryPool_; }
    const VulkanMemoryPool& memoryPool() const { return memoryPool_; }

    /// Single-use command buffer helpers for transfers and layout transitions.
    /// endSingleTimeCommands submits through the queue owner and waits for
    /// that submission only (not the device). Main thread.
    VkCommandBuffer beginSingleTimeCommands() const;
    void endSingleTimeCommands(VkCommandBuffer commandBuffer) const;

    /// Memory type index for `typeFilter` with all of `properties`, or
    /// kNoMemoryType (logged) when the device has none.
    static constexpr uint32_t kNoMemoryType = UINT32_MAX;
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    /// Pooled memory allocation methods (using VulkanMemoryPool)
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory,
                      VkDeviceSize& outOffset, uint64_t& outAllocId,
                      void*& outMappedData);

    void destroyBuffer(VkBuffer buffer, uint64_t allocId);

    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& memory,
                     VkDeviceSize& outOffset, uint64_t& outAllocId,
                     uint32_t mipLevels = 1, uint32_t arrayLayers = 1,
                     VkImageCreateFlags flags = 0);

    void destroyImage(VkImage image, uint64_t allocId);

    /// Legacy dedicated memory allocation helpers (allocates dedicated VkDeviceMemory)
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& bufferMemory) const;

    bool createImage(uint32_t width, uint32_t height, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory,
                     uint32_t mipLevels = 1, uint32_t arrayLayers = 1,
                     VkImageCreateFlags flags = 0) const;

    void copyBufferToImage(VkBuffer buffer, VkImage image,
                           uint32_t width, uint32_t height,
                           VkCommandBuffer cmd = VK_NULL_HANDLE,
                           uint32_t mipLevel = 0,
                           uint32_t baseArrayLayer = 0,
                           uint32_t layerCount = 1) const;

    void copyImageToBuffer(VkImage image, VkBuffer buffer,
                           uint32_t width, uint32_t height,
                           VkCommandBuffer cmd = VK_NULL_HANDLE) const;

    /// Layout transition with stage/access masks derived from the two layouts
    /// (render::cmdTransitionImage); the aspect follows from `format`.
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               VkCommandBuffer cmd = VK_NULL_HANDLE,
                               uint32_t mipLevels = 1, uint32_t baseMipLevel = 0,
                               uint32_t layerCount = 1, uint32_t baseArrayLayer = 0) const;

    /// Wait for everything submitted through the queue owner so far.
    void waitIdle() const;

    bool isHeadless() const { return config_.headless; }
    bool isValid() const { return device_ != VK_NULL_HANDLE; }

    /// Enumerate available queue families for a physical device, testing presentation against `surface` if provided.
    static VulkanQueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface = VK_NULL_HANDLE);

    /// Whether the present queue family can present to `surface` (a surface
    /// on a window other than the one the device was chosen for).
    bool canPresentTo(VkSurfaceKHR surface) const;

private:
    bool createInstance();
    bool setupDebugMessenger();
    bool selectPhysicalDevice(VkSurfaceKHR compatibleSurface);
    bool deviceMeetsRequirements(VkPhysicalDevice dev, const VkPhysicalDeviceProperties& props) const;
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

    uint32_t apiVersion_ = 0;
    bool synchronization2_ = false;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;  // single-time commands, guarded below
    mutable std::mutex commandPoolMutex_;
    mutable VulkanMemoryPool memoryPool_;
    // Declared after the memory pool: destroyed before it (frames free their
    // upload chunks back into the pool).
    mutable VulkanQueue queue_;  // thread-safe; const helpers submit through it
    VulkanFrames frames_;
};

} // namespace bro::render
