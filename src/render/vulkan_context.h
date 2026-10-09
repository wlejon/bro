#pragma once

#include "render/vulkan_frames.h"
#include "render/vulkan_memory_pool.h"
#include "render/vulkan_pipeline_cache.h"
#include "render/vulkan_queue.h"
#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bro::platform { class Window; }

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
    bool init(platform::Window* presentTarget = nullptr);

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
    /// Line widths other than 1 (enabled whenever the device has them;
    /// MoltenVK does not).
    bool wideLines() const { return deviceFeatures_.wideLines == VK_TRUE; }
    /// Point sizes other than 1 (enabled whenever the device has them).
    bool largePoints() const { return deviceFeatures_.largePoints == VK_TRUE; }
    /// The core features the device was created with.
    const VkPhysicalDeviceFeatures& features() const { return deviceFeatures_; }
    /// The instance and device extensions that were enabled (for libraries
    /// that share the device and must know what it offers, like Skia).
    const std::vector<std::string>& enabledInstanceExtensions() const { return instanceExtensions_; }
    const std::vector<std::string>& enabledDeviceExtensions() const { return deviceExtensions_; }
    /// Instance-rate vertex divisors other than 1 (VK_EXT_vertex_attribute_divisor).
    bool vertexAttributeDivisor() const { return vertexAttributeDivisor_; }
    /// Primitive restart in list topologies (VK_EXT_primitive_topology_list_restart).
    bool primitiveListRestart() const { return listRestart_; }
    /// 2D views of a 3D image's slices, to render into them (false only on
    /// portability-subset devices without imageView2DOn3DImage).
    bool imageView2DOn3D() const { return imageView2DOn3D_; }
    /// Depth-compare samplers (false only on portability-subset devices
    /// without mutableComparisonSamplers).
    bool comparisonSamplers() const { return mutableComparisonSamplers_; }

    VulkanQueue& queue() { return queue_; }
    VulkanFrames& frames() { return frames_; }
    /// The device's pipeline cache, persisted across runs; pass it to every
    /// vkCreate*Pipelines.
    VkPipelineCache pipelineCache() const { return pipelineCache_.handle(); }
    const VulkanPipelineCache& persistentPipelineCache() const { return pipelineCache_; }

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
                     VkImageCreateFlags flags = 0,
                     VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

    /// Any image (3D, mutable, ...) in pooled memory.
    bool createImage(const VkImageCreateInfo& info, VkMemoryPropertyFlags properties, VkImage& image,
                     VkDeviceMemory& memory, VkDeviceSize& outOffset, uint64_t& outAllocId);

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
    std::vector<std::string> instanceExtensions_;
    std::vector<std::string> deviceExtensions_;

    uint32_t apiVersion_ = 0;
    bool synchronization2_ = false;
    bool vertexAttributeDivisor_ = false;
    bool listRestart_ = false;
    bool imageView2DOn3D_ = true;
    bool mutableComparisonSamplers_ = true;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;  // single-time commands, guarded below
    mutable std::mutex commandPoolMutex_;
    mutable VulkanMemoryPool memoryPool_;
    // Declared after the memory pool: destroyed before it (frames free their
    // upload chunks back into the pool).
    mutable VulkanQueue queue_;  // thread-safe; const helpers submit through it
    VulkanFrames frames_;
    VulkanPipelineCache pipelineCache_;
};

} // namespace bro::render
