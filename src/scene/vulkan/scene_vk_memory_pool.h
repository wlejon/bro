#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace bro::scene::vk {

struct FreeRange {
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
};

struct MemoryBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t memoryTypeIndex = 0;
    VkMemoryPropertyFlags properties = 0;
    bool isHostVisible = false;
    void* mappedBase = nullptr;
    std::vector<FreeRange> freeRanges;
    size_t activeAllocations = 0;
};

struct AllocRecord {
    uint64_t id = 0;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* mappedData = nullptr;
    bool isDedicated = false;
    bool isHostVisible = false;
    uint32_t poolKey = 0;
    MemoryBlock* blockPtr = nullptr;
};

struct BlockPool {
    std::vector<std::unique_ptr<MemoryBlock>> blocks;
};

/// Statistics for Vulkan memory sub-allocations and pooled blocks.
struct SceneVkAllocatorStats {
    size_t totalAllocatedBytes = 0;
    size_t activeBlockCount = 0;
    size_t activeAllocationCount = 0;
    size_t dedicatedAllocationCount = 0;
};

/// High-performance chunked block sub-allocator for Vulkan buffers and images.
/// Pools allocations into 16 MB VkDeviceMemory chunks to prevent exceeding maxMemoryAllocationCount.
class SceneVkMemoryPool {
public:
    static constexpr VkDeviceSize kDefaultBlockSize = 16 * 1024 * 1024ULL;
    static constexpr VkDeviceSize kDedicatedThreshold = 8 * 1024 * 1024ULL;

    SceneVkMemoryPool() = default;
    ~SceneVkMemoryPool() = default;

    SceneVkMemoryPool(const SceneVkMemoryPool&) = delete;
    SceneVkMemoryPool& operator=(const SceneVkMemoryPool&) = delete;

    void cleanup(VkDevice device);

    bool allocate(VkDevice device, VkDeviceSize size, VkDeviceSize alignment,
                  uint32_t memoryTypeIndex, VkMemoryPropertyFlags properties, bool isImage,
                  uint64_t& outId, VkDeviceMemory& outMemory,
                  VkDeviceSize& outOffset, void*& outMappedData);

    void free(VkDevice device, uint64_t allocId);

    SceneVkAllocatorStats stats() const;

private:
    mutable std::mutex mutex_;
    uint64_t nextAllocId_ = 0;
    std::unordered_map<uint32_t, BlockPool> pools_;
    std::unordered_map<uint64_t, AllocRecord> allocations_;
};

} // namespace bro::scene::vk
