#include "render/vulkan_frames.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include <algorithm>

namespace bro::render {

namespace {

constexpr VkDeviceSize kUploadChunkSize = 4ull * 1024 * 1024;
constexpr uint32_t kDescriptorSetsPerPool = 256;

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

} // namespace

VulkanFrames::~VulkanFrames() {
    shutdown();
}

bool VulkanFrames::init(VulkanContext& context) {
    context_ = &context;
    const auto& limits = context.deviceProperties().limits;
    minUploadAlignment_ = std::max<VkDeviceSize>({16, limits.minUniformBufferOffsetAlignment,
                                                  limits.minStorageBufferOffsetAlignment,
                                                  limits.optimalBufferCopyOffsetAlignment,
                                                  limits.nonCoherentAtomSize});

    for (auto& slot : slots_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = static_cast<uint32_t>(context.queueFamilies().graphicsFamily);
        if (vkCreateCommandPool(context.device(), &poolInfo, nullptr, &slot.commandPool) != VK_SUCCESS) {
            LOG_ERROR("VulkanFrames: failed to create a frame command pool");
            return false;
        }
    }
    return true;
}

void VulkanFrames::shutdown() {
    if (!context_) return;
    VkDevice device = context_->device();
    queue().waitIdle();
    frameEndHooks_.clear();
    for (auto& fn : pending_) fn();
    pending_.clear();
    for (auto& slot : slots_) {
        for (auto& fn : slot.deferred) fn();
        slot.deferred.clear();
        for (auto& chunk : slot.uploads) destroyUploadChunk(chunk);
        slot.uploads.clear();
        for (VkDescriptorPool pool : slot.descriptorPools) vkDestroyDescriptorPool(device, pool, nullptr);
        slot.descriptorPools.clear();
        if (slot.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, slot.commandPool, nullptr);
            slot.commandPool = VK_NULL_HANDLE;
        }
        slot.commandBuffers.clear();
    }
    context_ = nullptr;
}

VulkanQueue& VulkanFrames::queue() {
    return context_->queue();
}

void VulkanFrames::beginFrame() {
    if (serial_ > 0) {
        // Hooks submit work still open in the closing frame. Iterate a copy: a
        // hook may remove itself (or another) while running.
        auto hooks = frameEndHooks_;
        for (auto& [id, hook] : hooks) hook();
        Slot& closing = slots_[frameIndex()];
        if (closing.openCommandBuffers > 0) {
            LOG_ERROR("VulkanFrames: frame %llu ended with %u command buffer(s) never submitted",
                      static_cast<unsigned long long>(serial_), closing.openCommandBuffers);
        }
        closing.ticket = queue().retireTicket();
        for (auto& fn : pending_) closing.deferred.push_back(std::move(fn));
        pending_.clear();
    }
    ++serial_;
    recycle(slots_[frameIndex()]);
}

void VulkanFrames::recycle(Slot& slot) {
    queue().wait(slot.ticket);
    for (auto& fn : slot.deferred) fn();
    slot.deferred.clear();

    VkDevice device = context_->device();
    vkResetCommandPool(device, slot.commandPool, 0);
    slot.nextCommandBuffer = 0;
    slot.openCommandBuffers = 0;

    // A frame that needed more than one upload chunk gets one chunk big
    // enough for all of it next time, so the arena settles at one buffer.
    if (slot.uploads.size() > 1) {
        VkDeviceSize total = 0;
        for (auto& chunk : slot.uploads) {
            total += chunk.size;
            destroyUploadChunk(chunk);
        }
        slot.uploads.clear();
        addUploadChunk(slot, total);
    }
    for (auto& chunk : slot.uploads) chunk.used = 0;

    for (VkDescriptorPool pool : slot.descriptorPools) vkResetDescriptorPool(device, pool, 0);
    slot.activeDescriptorPool = 0;
}

VkCommandBuffer VulkanFrames::beginCommands() {
    ensureFrame();
    Slot& slot = slots_[frameIndex()];
    if (slot.nextCommandBuffer == slot.commandBuffers.size()) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = slot.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(context_->device(), &allocInfo, &cmd) != VK_SUCCESS) {
            LOG_ERROR("VulkanFrames: failed to allocate a frame command buffer");
            return VK_NULL_HANDLE;
        }
        slot.commandBuffers.push_back(cmd);
    }
    VkCommandBuffer cmd = slot.commandBuffers[slot.nextCommandBuffer++];
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    ++slot.openCommandBuffers;
    return cmd;
}

uint64_t VulkanFrames::submit(VkCommandBuffer cmd, std::vector<SemaphoreWait> waits,
                              std::vector<SemaphoreSignal> signals) {
    Slot& slot = slots_[frameIndex()];
    if (slot.openCommandBuffers > 0) --slot.openCommandBuffers;
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
        LOG_ERROR("VulkanFrames: vkEndCommandBuffer failed");
        return 0;
    }
    QueueSubmit batch;
    batch.commandBuffers.push_back(cmd);
    batch.waits = std::move(waits);
    batch.signals = std::move(signals);
    return queue().submit(batch);
}

bool VulkanFrames::addUploadChunk(Slot& slot, VkDeviceSize minSize) {
    UploadChunk chunk;
    chunk.size = std::max(kUploadChunkSize, alignUp(minSize, kUploadChunkSize));
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (!context_->createBuffer(chunk.size, usage,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                chunk.buffer, chunk.memory, chunk.memoryOffset, chunk.allocId,
                                chunk.mapped) || !chunk.mapped) {
        LOG_ERROR("VulkanFrames: failed to create a %llu-byte upload chunk",
                  static_cast<unsigned long long>(chunk.size));
        destroyUploadChunk(chunk);
        return false;
    }
    slot.uploads.push_back(chunk);
    return true;
}

void VulkanFrames::destroyUploadChunk(UploadChunk& chunk) {
    if (chunk.buffer != VK_NULL_HANDLE || chunk.allocId != 0)
        context_->destroyBuffer(chunk.buffer, chunk.allocId);
    chunk = UploadChunk{};
}

UploadSlice VulkanFrames::allocUpload(VkDeviceSize size, VkDeviceSize alignment) {
    ensureFrame();
    Slot& slot = slots_[frameIndex()];
    const VkDeviceSize align = std::max(alignment, minUploadAlignment_);
    UploadChunk* chunk = slot.uploads.empty() ? nullptr : &slot.uploads.back();
    if (!chunk || alignUp(chunk->used, align) + size > chunk->size) {
        if (!addUploadChunk(slot, size)) return {};
        chunk = &slot.uploads.back();
    }
    UploadSlice slice;
    slice.buffer = chunk->buffer;
    slice.offset = alignUp(chunk->used, align);
    slice.size = size;
    slice.mapped = static_cast<char*>(chunk->mapped) + slice.offset;
    chunk->used = slice.offset + size;
    return slice;
}

VkDescriptorPool VulkanFrames::createDescriptorPool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSetsPerPool * 4},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescriptorSetsPerPool * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kDescriptorSetsPerPool},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescriptorSetsPerPool},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kDescriptorSetsPerPool},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kDescriptorSetsPerPool},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kDescriptorSetsPerPool / 2},
    };
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = kDescriptorSetsPerPool;
    info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(context_->device(), &info, nullptr, &pool) != VK_SUCCESS) {
        LOG_ERROR("VulkanFrames: failed to create a frame descriptor pool");
        return VK_NULL_HANDLE;
    }
    return pool;
}

VkDescriptorSet VulkanFrames::allocDescriptorSet(VkDescriptorSetLayout layout) {
    ensureFrame();
    Slot& slot = slots_[frameIndex()];
    for (;;) {
        const bool fresh = slot.activeDescriptorPool == slot.descriptorPools.size();
        if (fresh) {
            VkDescriptorPool pool = createDescriptorPool();
            if (pool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            slot.descriptorPools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = slot.descriptorPools[slot.activeDescriptorPool];
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult res = vkAllocateDescriptorSets(context_->device(), &info, &set);
        if (res == VK_SUCCESS) return set;
        if (fresh || (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL)) {
            LOG_ERROR("VulkanFrames: vkAllocateDescriptorSets failed (%d)", res);
            return VK_NULL_HANDLE;
        }
        ++slot.activeDescriptorPool;  // this pool is full; move to (or make) the next
    }
}

void VulkanFrames::defer(std::function<void()> destroy) {
    pending_.push_back(std::move(destroy));
}

VulkanFrames::HookId VulkanFrames::addFrameEndHook(std::function<void()> hook) {
    const HookId id = nextHookId_++;
    frameEndHooks_.emplace_back(id, std::move(hook));
    return id;
}

void VulkanFrames::removeFrameEndHook(HookId id) {
    frameEndHooks_.erase(std::remove_if(frameEndHooks_.begin(), frameEndHooks_.end(),
                                        [id](const auto& h) { return h.first == id; }),
                         frameEndHooks_.end());
}

} // namespace bro::render
