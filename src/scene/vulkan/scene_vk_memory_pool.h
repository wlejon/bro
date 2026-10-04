#pragma once

#include "render/vulkan_memory_pool.h"

namespace bro::scene::vk {

using FreeRange = render::VulkanFreeRange;
using MemoryBlock = render::VulkanMemoryBlock;
using AllocRecord = render::VulkanAllocRecord;
using BlockPool = render::VulkanBlockPool;

using VulkanMemoryPool = render::VulkanMemoryPool;
using SceneVkMemoryPool = render::VulkanMemoryPool;
using VulkanAllocatorStats = render::VulkanAllocatorStats;
using SceneVkAllocatorStats = render::VulkanAllocatorStats;

} // namespace bro::scene::vk
