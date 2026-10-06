#include "render/vulkan_memory_pool.h"
#include "util/log.h"

#include <algorithm>

namespace bro::render {

void VulkanMemoryPool::cleanup(VkDevice device) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Free all active dedicated allocations
    for (auto& [id, record] : allocations_) {
        if (record.isDedicated && record.memory != VK_NULL_HANDLE) {
            if (record.mappedData) {
                vkUnmapMemory(device, record.memory);
            }
            vkFreeMemory(device, record.memory, nullptr);
            record.memory = VK_NULL_HANDLE;
        }
    }
    allocations_.clear();

    // Free all pooled memory blocks across all pools
    for (auto& [key, p] : pools_) {
        for (auto& block : p.blocks) {
            if (block && block->memory != VK_NULL_HANDLE) {
                if (block->isHostVisible && block->mappedBase) {
                    vkUnmapMemory(device, block->memory);
                    block->mappedBase = nullptr;
                }
                vkFreeMemory(device, block->memory, nullptr);
                block->memory = VK_NULL_HANDLE;
            }
        }
        p.blocks.clear();
    }
    pools_.clear();
}

VulkanAllocatorStats VulkanMemoryPool::stats() const {
    VulkanAllocatorStats s{};
    std::lock_guard<std::mutex> lock(mutex_);
    s.activeAllocationCount = allocations_.size();
    for (const auto& [id, record] : allocations_) {
        s.totalAllocatedBytes += record.size;
        if (record.isDedicated) {
            s.dedicatedAllocationCount++;
        }
    }
    for (const auto& [key, p] : pools_) {
        s.activeBlockCount += p.blocks.size();
    }
    return s;
}

bool VulkanMemoryPool::allocate(VkDevice device, VkDeviceSize size, VkDeviceSize alignment,
                                uint32_t memoryTypeIndex, VkMemoryPropertyFlags properties, bool isImage,
                                uint64_t& outId, VkDeviceMemory& outMemory,
                                VkDeviceSize& outOffset, void*& outMappedData) {
    if (size == 0) return false;
    if (alignment < 1) alignment = 1;

    std::lock_guard<std::mutex> lock(mutex_);

    // Dedicated allocation for large resources
    if (size >= kDedicatedThreshold) {
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = size;
        allocInfo.memoryTypeIndex = memoryTypeIndex;

        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &mem) != VK_SUCCESS) {
            LOG_ERROR("VulkanMemoryPool: Failed dedicated allocation of %zu bytes", static_cast<size_t>(size));
            return false;
        }

        void* mapped = nullptr;
        bool isHostVis = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        if (isHostVis) {
            if (vkMapMemory(device, mem, 0, size, 0, &mapped) != VK_SUCCESS) {
                LOG_ERROR("VulkanMemoryPool: Failed to map host-visible dedicated memory");
                vkFreeMemory(device, mem, nullptr);
                return false;
            }
        }

        uint64_t id = ++nextAllocId_;
        VulkanAllocRecord record{};
        record.id = id;
        record.memory = mem;
        record.offset = 0;
        record.size = size;
        record.mappedData = mapped;
        record.isDedicated = true;
        record.isHostVisible = isHostVis;
        allocations_[id] = record;

        outId = id;
        outMemory = mem;
        outOffset = 0;
        outMappedData = mapped;
        return true;
    }

    uint32_t poolKey = (memoryTypeIndex << 1) | (isImage ? 1 : 0);
    VulkanBlockPool& p = pools_[poolKey];

    VulkanMemoryBlock* targetBlock = nullptr;
    size_t targetRangeIdx = 0;
    VkDeviceSize targetAlignedOffset = 0;

    const bool needHostVis = (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;

    for (auto& block : p.blocks) {
        if (needHostVis && !block->mappedBase) {
            if (vkMapMemory(device, block->memory, 0, block->size, 0, &block->mappedBase) == VK_SUCCESS) {
                block->isHostVisible = true;
            } else {
                continue;
            }
        }
        for (size_t r = 0; r < block->freeRanges.size(); ++r) {
            const auto& range = block->freeRanges[r];
            VkDeviceSize alignedOffset = (range.offset + (alignment - 1)) & ~(alignment - 1);
            if (alignedOffset >= range.offset && (alignedOffset + size) <= (range.offset + range.size)) {
                targetBlock = block.get();
                targetRangeIdx = r;
                targetAlignedOffset = alignedOffset;
                break;
            }
        }
        if (targetBlock) break;
    }

    if (!targetBlock) {
        VkDeviceSize blockSize = std::max(kDefaultBlockSize, size);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = blockSize;
        allocInfo.memoryTypeIndex = memoryTypeIndex;

        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (vkAllocateMemory(device, &allocInfo, nullptr, &mem) != VK_SUCCESS) {
            LOG_ERROR("VulkanMemoryPool: Failed to allocate pooled block of %zu bytes", static_cast<size_t>(blockSize));
            return false;
        }

        auto newBlock = std::make_unique<VulkanMemoryBlock>();
        newBlock->memory = mem;
        newBlock->size = blockSize;
        newBlock->memoryTypeIndex = memoryTypeIndex;
        newBlock->properties = properties;
        newBlock->isHostVisible = needHostVis;
        if (needHostVis) {
            if (vkMapMemory(device, mem, 0, blockSize, 0, &newBlock->mappedBase) != VK_SUCCESS) {
                LOG_ERROR("VulkanMemoryPool: Failed to map host-visible memory block");
                vkFreeMemory(device, mem, nullptr);
                return false;
            }
        }
        newBlock->freeRanges.push_back({0, blockSize});

        targetBlock = newBlock.get();
        targetRangeIdx = 0;
        targetAlignedOffset = 0;
        p.blocks.push_back(std::move(newBlock));
    }

    VulkanFreeRange origRange = targetBlock->freeRanges[targetRangeIdx];
    targetBlock->freeRanges.erase(targetBlock->freeRanges.begin() + targetRangeIdx);

    // Free space before aligned sub-range
    if (targetAlignedOffset > origRange.offset) {
        targetBlock->freeRanges.insert(
            targetBlock->freeRanges.begin() + targetRangeIdx,
            { origRange.offset, targetAlignedOffset - origRange.offset }
        );
        targetRangeIdx++;
    }

    // Free space after allocated sub-range
    VkDeviceSize allocEnd = targetAlignedOffset + size;
    VkDeviceSize rangeEnd = origRange.offset + origRange.size;
    if (allocEnd < rangeEnd) {
        targetBlock->freeRanges.insert(
            targetBlock->freeRanges.begin() + targetRangeIdx,
            { allocEnd, rangeEnd - allocEnd }
        );
    }

    targetBlock->activeAllocations++;

    void* mapped = nullptr;
    if (targetBlock->isHostVisible && targetBlock->mappedBase) {
        mapped = static_cast<char*>(targetBlock->mappedBase) + targetAlignedOffset;
    }

    uint64_t id = ++nextAllocId_;
    VulkanAllocRecord record{};
    record.id = id;
    record.memory = targetBlock->memory;
    record.offset = targetAlignedOffset;
    record.size = size;
    record.mappedData = mapped;
    record.isDedicated = false;
    record.isHostVisible = targetBlock->isHostVisible;
    record.poolKey = poolKey;
    record.blockPtr = targetBlock;
    allocations_[id] = record;

    outId = id;
    outMemory = targetBlock->memory;
    outOffset = targetAlignedOffset;
    outMappedData = mapped;
    return true;
}

void VulkanMemoryPool::free(VkDevice device, uint64_t allocId) {
    if (allocId == 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = allocations_.find(allocId);
    if (it == allocations_.end()) return;

    const VulkanAllocRecord& record = it->second;
    if (record.isDedicated) {
        if (record.mappedData) {
            vkUnmapMemory(device, record.memory);
        }
        vkFreeMemory(device, record.memory, nullptr);
        allocations_.erase(it);
        return;
    }

    VulkanMemoryBlock* block = record.blockPtr;
    if (block) {
        // Insert range maintaining sorted order
        VulkanFreeRange newRange{record.offset, record.size};
        auto insertPos = std::lower_bound(
            block->freeRanges.begin(), block->freeRanges.end(), newRange,
            [](const VulkanFreeRange& a, const VulkanFreeRange& b) { return a.offset < b.offset; }
        );
        block->freeRanges.insert(insertPos, newRange);

        // Coalesce adjacent free blocks
        for (size_t i = 0; i + 1 < block->freeRanges.size(); ) {
            if (block->freeRanges[i].offset + block->freeRanges[i].size == block->freeRanges[i + 1].offset) {
                block->freeRanges[i].size += block->freeRanges[i + 1].size;
                block->freeRanges.erase(block->freeRanges.begin() + (i + 1));
            } else {
                ++i;
            }
        }

        if (block->activeAllocations > 0) {
            block->activeAllocations--;
        }

        // If block is completely empty and there is more than 1 block in pool, prune it
        if (block->activeAllocations == 0) {
            auto poolIt = pools_.find(record.poolKey);
            if (poolIt != pools_.end()) {
                size_t emptyCount = 0;
                for (const auto& b : poolIt->second.blocks) {
                    if (b->activeAllocations == 0) emptyCount++;
                }
                if (emptyCount > 1) {
                    auto& vec = poolIt->second.blocks;
                    for (auto bIt = vec.begin(); bIt != vec.end(); ++bIt) {
                        if (bIt->get() == block) {
                            if (block->isHostVisible && block->mappedBase) {
                                vkUnmapMemory(device, block->memory);
                                block->mappedBase = nullptr;
                            }
                            vkFreeMemory(device, block->memory, nullptr);
                            vec.erase(bIt);
                            break;
                        }
                    }
                }
            }
        }
    }

    allocations_.erase(it);
}

} // namespace bro::render
