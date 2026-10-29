#pragma once

#include "render/vulkan_compute.h"
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
    /// A family with compute and no graphics when the device has one (its
    /// queues run beside the graphics queue: VulkanContext::asyncComputeQueue),
    /// else the graphics family.
    int computeFamily = -1;
    int transferFamily = -1;

    bool isComplete(bool needPresent = true) const {
        return graphicsFamily >= 0 && (!needPresent || presentFamily >= 0);
    }
};

/// What compute shaders compiled outside bro (brokernel's SPIR-V) may use:
/// the device features behind their capabilities, each enabled where the
/// device has it, and the limits a pipeline is checked against.
struct VulkanComputeFeatures {
    bool bufferDeviceAddress = false;   // PhysicalStorageBufferAddresses: buffers reached by address
    bool shaderInt64 = false;
    bool shaderFloat64 = false;
    bool shaderInt16 = false;
    bool storageBuffer8Bit = false;
    bool storageBuffer16Bit = false;
    bool bufferInt64Atomics = false;
    bool sharedInt64Atomics = false;
    bool bufferFloat32AtomicAdd = false;  // VK_EXT_shader_atomic_float
    bool sharedFloat32AtomicAdd = false;
    bool bufferFloat64AtomicAdd = false;
    bool subgroupBasic = false;           // in the compute stage
    bool subgroupShuffle = false;
    uint32_t subgroupSize = 0;
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
    /// VK_KHR_external_memory_win32 is enabled (Windows, when the device has it).
    bool hasExternalMemoryWin32() const { return externalMemoryWin32_; }
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
    /// The compute features that were enabled (VulkanComputeFeatures).
    const VulkanComputeFeatures& computeFeatures() const { return computeFeatures_; }
    /// The device address of `buffer` (made with
    /// VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT); 0 without bufferDeviceAddress.
    uint64_t bufferAddress(VkBuffer buffer) const;

    VulkanQueue& queue() { return queue_; }
    /// The queue owner of the transfer family (the copy engine) when the
    /// device has one apart from graphics, else null. Long copies submitted
    /// here run beside the frames: a frame's work on queue() is never ordered
    /// behind them. Its tickets are its own timeline's, not queue()'s, and an
    /// image it writes changes queue family ownership (release here, acquire
    /// on queue()).
    VulkanQueue* uploadQueue() { return hasUploadQueue_ ? &uploadQueue_ : nullptr; }
    /// The queue owner of a compute family without graphics, when the device
    /// has one, else null: then work meant
    /// for it records into the graphics stream instead. Its tickets are its
    /// own timeline's; a frame orders against it by timeline waits at
    /// submission boundaries. Buffers both queues touch are created shared
    /// (createBuffer's `shared`), so they need no ownership transfers; an
    /// image moves by release on one family and acquire on the other.
    VulkanQueue* asyncComputeQueue() { return hasAsyncCompute_ ? &asyncCompute_ : nullptr; }
    VulkanFrames& frames() { return frames_; }
    /// Compute work in the graphics stream over buffers reached by address
    /// (VulkanCompute): what runs kernels compiled outside bro.
    VulkanCompute& compute() { return compute_; }
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

    /// Pooled memory allocation methods (using VulkanMemoryPool). A `shared`
    /// buffer is concurrent between the graphics and async compute families
    /// (exclusive when there is no async compute queue).
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties,
                      VkBuffer& buffer, VkDeviceMemory& memory,
                      VkDeviceSize& outOffset, uint64_t& outAllocId,
                      void*& outMappedData, bool shared = false);

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

    /// Wait for everything submitted through the queue owners so far (the
    /// graphics queue's and the async compute queue's).
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
    bool externalMemoryWin32_ = false;
    bool vertexAttributeDivisor_ = false;
    bool listRestart_ = false;
    bool imageView2DOn3D_ = true;
    bool mutableComparisonSamplers_ = true;
    VulkanComputeFeatures computeFeatures_;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;  // single-time commands, guarded below
    mutable std::mutex commandPoolMutex_;
    mutable std::mutex resourceMutex_;
    mutable VulkanMemoryPool memoryPool_;
    // Declared after the memory pool: destroyed before it (frames free their
    // upload chunks back into the pool).
    mutable VulkanQueue queue_;  // thread-safe; const helpers submit through it
    VulkanQueue uploadQueue_;    // the transfer family's, when apart from graphics
    bool hasUploadQueue_ = false;
    mutable VulkanQueue asyncCompute_;   // a compute family's without graphics
    bool hasAsyncCompute_ = false;
    uint32_t computeQueueIndex_ = 0;     // in its family (1 when it shares the transfer family)
    VulkanFrames frames_;
    VulkanCompute compute_;
    VulkanPipelineCache pipelineCache_;
};

} // namespace bro::render
