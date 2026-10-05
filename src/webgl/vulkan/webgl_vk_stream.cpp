#include "webgl/vulkan/webgl_vk_stream.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include <algorithm>

namespace bro::webgl::vk {

namespace {

constexpr VkDeviceSize kUploadChunkSize = 4ull * 1024 * 1024;
// What one segment may use before the context is asked to submit. A segment
// is never refused memory — a single upload larger than this still gets it —
// the budget only decides when the stream rotates.
constexpr VkDeviceSize kUploadBudget = 32ull * 1024 * 1024;
constexpr uint32_t kDescriptorSetBudget = 4096;
constexpr uint32_t kDescriptorSetsPerPool = 256;

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) / a * a;
}

} // namespace

WebGLVkStream::WebGLVkStream(render::VulkanContext& context) : context_(context) {
    const auto& limits = context.deviceProperties().limits;
    minUploadAlignment_ = std::max<VkDeviceSize>({16, limits.minUniformBufferOffsetAlignment,
                                                  limits.optimalBufferCopyOffsetAlignment,
                                                  limits.nonCoherentAtomSize});
    for (Segment& segment : segments_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex = static_cast<uint32_t>(context.queueFamilies().graphicsFamily);
        if (vkCreateCommandPool(context.device(), &poolInfo, nullptr, &segment.commandPool) != VK_SUCCESS)
            LOG_ERROR("WebGLVkStream: failed to create a command pool");
    }
}

WebGLVkStream::~WebGLVkStream() {
    submit();
    context_.queue().wait(lastTicket_);
    VkDevice device = context_.device();
    for (Segment& segment : segments_) {
        for (auto& fn : segment.deferred) fn();
        for (auto& chunk : segment.uploads) destroyUploadChunk(chunk);
        for (VkDescriptorPool pool : segment.descriptorPools) vkDestroyDescriptorPool(device, pool, nullptr);
        if (segment.commandPool != VK_NULL_HANDLE) vkDestroyCommandPool(device, segment.commandPool, nullptr);
        segment = Segment{};
    }
}

VkCommandBuffer WebGLVkStream::commands() {
    if (cmd_ != VK_NULL_HANDLE) return cmd_;
    Segment& segment = open();
    if (segment.nextCommandBuffer == segment.commandBuffers.size()) {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = segment.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(context_.device(), &allocInfo, &cmd) != VK_SUCCESS) {
            LOG_ERROR("WebGLVkStream: failed to allocate a command buffer");
            return VK_NULL_HANDLE;
        }
        segment.commandBuffers.push_back(cmd);
    }
    VkCommandBuffer cmd = segment.commandBuffers[segment.nextCommandBuffer++];
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    cmd_ = cmd;
    return cmd_;
}

bool WebGLVkStream::submit() {
    Segment& segment = open();
    bool ok = true;
    if (cmd_ != VK_NULL_HANDLE) {
        vkEndCommandBuffer(cmd_);
        render::QueueSubmit batch;
        batch.commandBuffers.push_back(cmd_);
        cmd_ = VK_NULL_HANDLE;
        const uint64_t ticket = context_.queue().submit(batch);
        if (ticket == 0) {
            LOG_ERROR("WebGLVkStream: submitting recorded commands failed");
            ok = false;
        } else {
            lastTicket_ = ticket;
        }
    }
    // A segment with nothing allocated and nothing to release stays open: it
    // has no resources a rotation would recycle.
    const bool used = segment.nextCommandBuffer > 0 || segment.uploadBytes > 0 ||
                      segment.descriptorSets > 0 || !segment.deferred.empty();
    if (!used) return ok;
    // Everything the segment's commands touched was submitted by now, so its
    // memory and deferred destructions retire with the latest ticket.
    segment.ticket = lastTicket_;
    index_ = (index_ + 1) % kSegments;
    ++serial_;
    recycle(open());
    return ok;
}

bool WebGLVkStream::wantsSubmit() const {
    const Segment& segment = segments_[index_];
    return segment.uploadBytes > kUploadBudget || segment.descriptorSets > kDescriptorSetBudget;
}

void WebGLVkStream::recycle(Segment& segment) {
    if (segment.ticket != 0) context_.queue().wait(segment.ticket);
    segment.ticket = 0;
    for (auto& fn : segment.deferred) fn();
    segment.deferred.clear();

    VkDevice device = context_.device();
    if (segment.nextCommandBuffer > 0) vkResetCommandPool(device, segment.commandPool, 0);
    segment.nextCommandBuffer = 0;

    // Keep one chunk's worth of upload memory; anything a burst added beyond
    // that goes, so a single huge upload does not pin its memory for good.
    while (segment.uploads.size() > 1) {
        destroyUploadChunk(segment.uploads.back());
        segment.uploads.pop_back();
    }
    if (!segment.uploads.empty() && segment.uploads.front().size > kUploadChunkSize) {
        destroyUploadChunk(segment.uploads.front());
        segment.uploads.clear();
    }
    for (auto& chunk : segment.uploads) chunk.used = 0;
    segment.uploadBytes = 0;

    for (VkDescriptorPool pool : segment.descriptorPools) vkResetDescriptorPool(device, pool, 0);
    segment.activeDescriptorPool = 0;
    segment.descriptorSets = 0;
}

bool WebGLVkStream::addUploadChunk(Segment& segment, VkDeviceSize minSize) {
    UploadChunk chunk;
    chunk.size = std::max(kUploadChunkSize, alignUp(minSize, kUploadChunkSize));
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (!context_.createBuffer(chunk.size, usage,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               chunk.buffer, chunk.memory, chunk.memoryOffset, chunk.allocId, chunk.mapped) ||
        !chunk.mapped) {
        LOG_ERROR("WebGLVkStream: failed to create a %llu-byte upload chunk",
                  static_cast<unsigned long long>(chunk.size));
        destroyUploadChunk(chunk);
        return false;
    }
    segment.uploads.push_back(chunk);
    return true;
}

void WebGLVkStream::destroyUploadChunk(UploadChunk& chunk) {
    if (chunk.buffer != VK_NULL_HANDLE || chunk.allocId != 0) context_.destroyBuffer(chunk.buffer, chunk.allocId);
    chunk = UploadChunk{};
}

render::UploadSlice WebGLVkStream::allocUpload(VkDeviceSize size, VkDeviceSize alignment) {
    Segment& segment = open();
    const VkDeviceSize align = std::max(alignment, minUploadAlignment_);
    UploadChunk* chunk = nullptr;
    for (auto& c : segment.uploads) {
        if (alignUp(c.used, align) + size <= c.size) {
            chunk = &c;
            break;
        }
    }
    if (!chunk) {
        if (!addUploadChunk(segment, size)) return {};
        chunk = &segment.uploads.back();
    }
    render::UploadSlice slice;
    slice.buffer = chunk->buffer;
    slice.offset = alignUp(chunk->used, align);
    slice.size = size;
    slice.mapped = static_cast<char*>(chunk->mapped) + slice.offset;
    chunk->used = slice.offset + size;
    segment.uploadBytes += size;
    return slice;
}

VkDescriptorPool WebGLVkStream::createDescriptorPool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSetsPerPool * 8},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kDescriptorSetsPerPool * 4},
    };
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = kDescriptorSetsPerPool;
    info.poolSizeCount = static_cast<uint32_t>(std::size(sizes));
    info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(context_.device(), &info, nullptr, &pool) != VK_SUCCESS) {
        LOG_ERROR("WebGLVkStream: failed to create a descriptor pool");
        return VK_NULL_HANDLE;
    }
    return pool;
}

VkDescriptorSet WebGLVkStream::allocDescriptorSet(VkDescriptorSetLayout layout) {
    Segment& segment = open();
    VkDescriptorSetAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &layout;
    // Try the active pool, then a fresh one: a pool runs out of sets or of
    // descriptors of one type, and either way the next pool has room.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (segment.activeDescriptorPool == segment.descriptorPools.size()) {
            VkDescriptorPool pool = createDescriptorPool();
            if (pool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            segment.descriptorPools.push_back(pool);
        }
        info.descriptorPool = segment.descriptorPools[segment.activeDescriptorPool];
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(context_.device(), &info, &set) == VK_SUCCESS) {
            ++segment.descriptorSets;
            return set;
        }
        ++segment.activeDescriptorPool;
    }
    LOG_ERROR("WebGLVkStream: failed to allocate a descriptor set");
    return VK_NULL_HANDLE;
}

void WebGLVkStream::defer(std::function<void()> destroy) {
    open().deferred.push_back(std::move(destroy));
}

} // namespace bro::webgl::vk
