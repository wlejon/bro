#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bro::render {

struct VulkanFreeRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct VulkanMemoryBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t memoryTypeIndex = 0;
    VkMemoryPropertyFlags properties = 0;
    bool isHostVisible = false;
    void* mappedBase = nullptr;
    std::vector<VulkanFreeRange> freeRanges;
    size_t activeAllocations = 0;
};

struct VulkanAllocRecord {
    uint64_t id = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* mappedData = nullptr;
    bool isDedicated = false;
    bool isHostVisible = false;
    uint32_t poolKey = 0;
    VulkanMemoryBlock* blockPtr = nullptr;
};

struct VulkanBlockPool {
    std::vector<std::unique_ptr<VulkanMemoryBlock>> blocks;
};

/// Statistics for Vulkan memory sub-allocations and pooled blocks.
struct VulkanAllocatorStats {
    size_t totalAllocatedBytes = 0;
    size_t activeBlockCount = 0;
    size_t activeAllocationCount = 0;
    size_t dedicatedAllocationCount = 0;
    // VkDeviceMemory held, by whether it is mapped into the process (host
    // visible: the process's own committed memory) or not.
    size_t hostVisibleBytes = 0;
    size_t deviceOnlyBytes = 0;
};

/// High-performance chunked block sub-allocator for Vulkan buffers and images.
/// Pools allocations into 16 MB VkDeviceMemory chunks to prevent exceeding maxMemoryAllocationCount.
class VulkanMemoryPool {
public:
    static constexpr VkDeviceSize kDefaultBlockSize = 16 * 1024 * 1024ULL;
    static constexpr VkDeviceSize kDedicatedThreshold = 8 * 1024 * 1024ULL;

    VulkanMemoryPool() = default;
    ~VulkanMemoryPool() = default;

    VulkanMemoryPool(const VulkanMemoryPool&) = delete;
    VulkanMemoryPool& operator=(const VulkanMemoryPool&) = delete;

    void cleanup(VkDevice device);

    bool allocate(VkDevice device, VkDeviceSize size, VkDeviceSize alignment,
                  uint32_t memoryTypeIndex, VkMemoryPropertyFlags properties, bool isImage,
                  uint64_t& outId, VkDeviceMemory& outMemory,
                  VkDeviceSize& outOffset, void*& outMappedData);

    void free(VkDevice device, uint64_t allocId);

    VulkanAllocatorStats stats() const;

private:
    mutable std::mutex mutex_;
    uint64_t nextAllocId_ = 0;
    std::unordered_map<uint32_t, VulkanBlockPool> pools_;
    std::unordered_map<uint64_t, VulkanAllocRecord> allocations_;
};

} // namespace bro::render
