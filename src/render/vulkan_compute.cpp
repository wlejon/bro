#include "render/vulkan_compute.h"

#include "render/vulkan_context.h"
#include "render/vulkan_util.h"
#include "util/log.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace bro::render {

namespace {

constexpr VkDeviceSize kBufferGranule = 256;
// Timestamp queries for timed dispatches, two a dispatch, in a ring.
constexpr uint32_t kTimedQueries = 8192;
// And for spans, two a span.
constexpr uint32_t kSpanQueries = 256;

constexpr VkAccessFlags kComputeAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
constexpr VkPipelineStageFlags kComputeStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;

void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags srcStages, VkAccessFlags srcAccess,
                   VkPipelineStageFlags dstStages, VkAccessFlags dstAccess) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStages, dstStages, 0, 1, &b, 0, nullptr, 0, nullptr);
}

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// ComputeBuffer / ComputePipeline

ComputeBuffer::~ComputeBuffer() {
    if (!owner_ || !owner_->context) return;
    owner_->buffers.erase(this);
    VulkanContext* ctx = owner_->context;
    ctx->frames().defer([ctx, buffer = buffer_, alloc = allocId_] { ctx->destroyBuffer(buffer, alloc); });
}

void ComputeBuffer::markLayers(std::span<const uint32_t> parts, uint32_t layers) {
    if (layers == 0) return;
    ++layerCount_;
    if (layerStamps_.size() != layers) layerStamps_.assign(layers, layerCount_);
    for (uint32_t p : parts)
        if (p < layers) layerStamps_[p] = layerCount_;
}

void ComputeBuffer::release() {
    if (owner_ && owner_->context) owner_->context->destroyBuffer(buffer_, allocId_);
    buffer_ = VK_NULL_HANDLE;
    allocId_ = 0;
    address_ = 0;
}

ComputeReadback::~ComputeReadback() {
    if (!owner_ || !owner_->context) return;
    owner_->readbacks.erase(this);
    VulkanContext* ctx = owner_->context;
    ctx->frames().defer([ctx, buffer = buffer_, alloc = allocId_] { ctx->destroyBuffer(buffer, alloc); });
}

void ComputeReadback::release() {
    if (owner_ && owner_->context) owner_->context->destroyBuffer(buffer_, allocId_);
    buffer_ = VK_NULL_HANDLE;
    allocId_ = 0;
    mapped_ = nullptr;
}

bool ComputeReadback::ready() const {
    return owner_ && owner_->context && buffer_ != VK_NULL_HANDLE && owner_->context->queue().isComplete(ticket_);
}

ComputeSpan::~ComputeSpan() {
    // Its queries are reset where the next span begins, after this one's.
    if (owner_ && owner_->context) owner_->freeSpans.push_back(query_);
}

bool ComputeSpan::ready() const {
    return owner_ && owner_->context && ticket_ && owner_->context->queue().isComplete(ticket_);
}

double ComputeSpan::ms() const {
    if (ms_ < 0.0 && ready()) {
        uint64_t stamps[2] = {0, 0};
        if (vkGetQueryPoolResults(owner_->context->device(), owner_->spanPool, query_, 2, sizeof(stamps), stamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            ms_ = static_cast<double>((stamps[1] - stamps[0]) & owner_->timestampMask) * owner_->timestampNs * 1e-6;
    }
    return ms_;
}

ComputePipeline::~ComputePipeline() {
    if (!owner_ || !owner_->context) return;
    owner_->pipelines.erase(this);
    VulkanContext* ctx = owner_->context;
    ctx->frames().defer([dev = ctx->device(), layout = layout_, pipeline = pipeline_] {
        vkDestroyPipeline(dev, pipeline, nullptr);
        vkDestroyPipelineLayout(dev, layout, nullptr);
    });
}

void ComputePipeline::release() {
    if (owner_ && owner_->context) {
        vkDestroyPipeline(owner_->context->device(), pipeline_, nullptr);
        vkDestroyPipelineLayout(owner_->context->device(), layout_, nullptr);
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// VulkanCompute

VulkanCompute::~VulkanCompute() { shutdown(); }

bool VulkanCompute::init(VulkanContext& context) {
    context_ = &context;
    owner_ = std::make_shared<ComputeOwner>();
    owner_->context = &context;
    // A frame's command buffers never outlive it: what is open goes now.
    frameHook_ = context.frames().addFrameEndHook([this] { flush(); });
    return true;
}

void VulkanCompute::shutdown() {
    if (!context_) return;
    flush();
    context_->frames().removeFrameEndHook(frameHook_);
    if (lastTicket_) context_->queue().wait(lastTicket_);
    for (ComputeBuffer* b : owner_->buffers) b->release();
    for (ComputePipeline* p : owner_->pipelines) p->release();
    for (ComputeReadback* r : owner_->readbacks) r->release();
    owner_->buffers.clear();
    owner_->pipelines.clear();
    owner_->readbacks.clear();
    if (owner_->spanPool != VK_NULL_HANDLE) vkDestroyQueryPool(context_->device(), owner_->spanPool, nullptr);
    owner_->spanPool = VK_NULL_HANDLE;
    owner_->context = nullptr;
    owner_.reset();
    if (readback_ != VK_NULL_HANDLE) context_->destroyBuffer(readback_, readbackAlloc_);
    readback_ = VK_NULL_HANDLE;
    readbackSize_ = 0;
    if (timePool_ != VK_NULL_HANDLE) vkDestroyQueryPool(context_->device(), timePool_, nullptr);
    timePool_ = VK_NULL_HANDLE;
    timing_ = false;
    timed_.clear();
    context_ = nullptr;
}

bool VulkanCompute::available() const {
    if (!context_) return false;
    const VulkanComputeFeatures& f = context_->computeFeatures();
    return f.bufferDeviceAddress && f.shaderInt64;
}

std::shared_ptr<ComputeBuffer> VulkanCompute::createBuffer(VkDeviceSize size, std::string* error) {
    if (!available()) {
        fail(error, "the GPU device cannot reach buffers by address");
        return nullptr;
    }
    const VkDeviceSize capacity = std::max(kBufferGranule, (size + kBufferGranule - 1) / kBufferGranule * kBufferGranule);
    std::shared_ptr<ComputeBuffer> b(new ComputeBuffer());
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* mapped = nullptr;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (!context_->createBuffer(capacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, b->buffer_, memory, offset,
                                b->allocId_, mapped)) {
        fail(error, "could not allocate a " + std::to_string(capacity) + "-byte GPU buffer");
        return nullptr;
    }
    b->owner_ = owner_;
    b->address_ = context_->bufferAddress(b->buffer_);
    b->size_ = size;
    b->capacity_ = capacity;
    owner_->buffers.insert(b.get());

    // It reads zero until written.
    VkCommandBuffer cmd = commands();
    vkCmdFillBuffer(cmd, b->buffer_, 0, VK_WHOLE_SIZE, 0);
    pendingBarrier_ = true;
    return b;
}

std::shared_ptr<ComputePipeline> VulkanCompute::createPipeline(const ComputePipelineDesc& desc, std::string* error) {
    if (!available()) {
        fail(error, "the GPU device cannot reach buffers by address");
        return nullptr;
    }
    if (desc.spirv.empty()) {
        fail(error, "no SPIR-V");
        return nullptr;
    }
    const VkDevice dev = context_->device();
    const VkPhysicalDeviceLimits& limits = context_->deviceProperties().limits;
    if (desc.pushConstantBytes > limits.maxPushConstantsSize) {
        fail(error, "push constants take " + std::to_string(desc.pushConstantBytes) + " bytes; the device allows " +
                        std::to_string(limits.maxPushConstantsSize));
        return nullptr;
    }
    const uint64_t threads = uint64_t{desc.localSize[0]} * desc.localSize[1] * desc.localSize[2];
    if (threads == 0 || threads > limits.maxComputeWorkGroupInvocations ||
        desc.localSize[0] > limits.maxComputeWorkGroupSize[0] || desc.localSize[1] > limits.maxComputeWorkGroupSize[1] ||
        desc.localSize[2] > limits.maxComputeWorkGroupSize[2]) {
        fail(error, "workgroup size " + std::to_string(desc.localSize[0]) + "x" + std::to_string(desc.localSize[1]) +
                        "x" + std::to_string(desc.localSize[2]) + " exceeds the device limits");
        return nullptr;
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = desc.spirv.size() * sizeof(uint32_t);
    smi.pCode = desc.spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smi, nullptr, &module) != VK_SUCCESS) {
        fail(error, "vkCreateShaderModule failed for '" + desc.entry + "'");
        return nullptr;
    }

    std::shared_ptr<ComputePipeline> p(new ComputePipeline());
    p->pushBytes_ = desc.pushConstantBytes;
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, desc.pushConstantBytes};
    VkPipelineLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.pushConstantRangeCount = desc.pushConstantBytes ? 1u : 0u;
    lci.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(dev, &lci, nullptr, &p->layout_) != VK_SUCCESS) {
        vkDestroyShaderModule(dev, module, nullptr);
        fail(error, "vkCreatePipelineLayout failed for '" + desc.entry + "'");
        return nullptr;
    }

    const VkSpecializationMapEntry entries[3] = {
        {0, 0, sizeof(uint32_t)}, {1, sizeof(uint32_t), sizeof(uint32_t)}, {2, 2 * sizeof(uint32_t), sizeof(uint32_t)}};
    VkSpecializationInfo spec{3, entries, sizeof(desc.localSize), desc.localSize};
    VkComputePipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = module;
    pci.stage.pName = desc.entry.c_str();
    pci.stage.pSpecializationInfo = desc.localSizeSpecConstants ? &spec : nullptr;
    pci.layout = p->layout_;
    const VkResult res = vkCreateComputePipelines(dev, context_->pipelineCache(), 1, &pci, nullptr, &p->pipeline_);
    vkDestroyShaderModule(dev, module, nullptr);
    if (res != VK_SUCCESS) {
        vkDestroyPipelineLayout(dev, p->layout_, nullptr);
        fail(error, "vkCreateComputePipelines failed for '" + desc.entry + "' (" + std::to_string(res) + ")");
        return nullptr;
    }
    p->owner_ = owner_;
    p->label_ = std::make_shared<const std::string>(desc.label.empty() ? desc.entry : desc.label);
    owner_->pipelines.insert(p.get());
    return p;
}

VkCommandBuffer VulkanCompute::commands() {
    if (open_ == VK_NULL_HANDLE) {
        context_->frames().ensureFrame();
        open_ = context_->frames().beginCommands();
        // After whatever was submitted before: draws reading a buffer this
        // work writes, and earlier writes it reads.
        memoryBarrier(open_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, kComputeStages,
                      kComputeAccess);
        pendingBarrier_ = false;
    } else if (pendingBarrier_) {
        barrier();
    }
    return open_;
}

void VulkanCompute::barrier() {
    memoryBarrier(open_, kComputeStages, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, kComputeStages,
                  kComputeAccess);
    pendingBarrier_ = false;
}

bool VulkanCompute::write(ComputeBuffer& buffer, VkDeviceSize offset, const void* data, VkDeviceSize size) {
    if (!context_ || buffer.buffer_ == VK_NULL_HANDLE || offset > buffer.size_ || size > buffer.size_ - offset) return false;
    if (size == 0) return true;
    UploadSlice slice = context_->frames().allocUpload(size, 4);
    if (!slice) return false;
    std::memcpy(slice.mapped, data, static_cast<size_t>(size));
    VkCommandBuffer cmd = commands();
    const VkBufferCopy copy{slice.offset, offset, size};
    vkCmdCopyBuffer(cmd, slice.buffer, buffer.buffer_, 1, &copy);
    pendingBarrier_ = true;
    return true;
}

bool VulkanCompute::dispatch(const ComputePipeline& pipeline, std::span<const uint8_t> push, const uint32_t groups[3]) {
    if (!context_ || pipeline.pipeline_ == VK_NULL_HANDLE || push.size() != pipeline.pushBytes_) return false;
    if (groups[0] == 0 || groups[1] == 0 || groups[2] == 0) return true;
    VkCommandBuffer cmd = commands();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline_);
    if (!push.empty()) {
        vkCmdPushConstants(cmd, pipeline.layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<uint32_t>(push.size()),
                           push.data());
    }
    // A pair of queries the ring still has free (none: this one goes untimed).
    const bool timed = timing_ && timePool_ != VK_NULL_HANDLE && timed_.size() < kTimedQueries / 2;
    const uint32_t query = nextQuery_;
    if (timed) {
        nextQuery_ = (nextQuery_ + 2) % kTimedQueries;
        vkCmdResetQueryPool(cmd, timePool_, query, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timePool_, query);
    }
    vkCmdDispatch(cmd, groups[0], groups[1], groups[2]);
    if (timed) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timePool_, query + 1);
        timed_.push_back({pipeline.label_, query, 0});
    }
    pendingBarrier_ = true;
    return true;
}

bool VulkanCompute::timestamps() {
    if (!context_) return false;
    if (owner_->timestampNs == 0.0) {
        owner_->timestampNs = -1.0;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(context_->physicalDevice(), &props);
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context_->physicalDevice(), &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context_->physicalDevice(), &familyCount, families.data());
        const int family = context_->queueFamilies().graphicsFamily;
        if (family >= 0 && static_cast<uint32_t>(family) < familyCount && families[family].timestampValidBits != 0 &&
            props.limits.timestampPeriod > 0.0f) {
            const uint32_t bits = families[family].timestampValidBits;
            owner_->timestampMask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
            owner_->timestampNs = props.limits.timestampPeriod;
        }
    }
    return owner_->timestampNs > 0.0;
}

std::shared_ptr<ComputeSpan> VulkanCompute::beginSpan() {
    if (!timestamps()) return nullptr;
    if (owner_->spanPool == VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kSpanQueries;
        if (vkCreateQueryPool(context_->device(), &info, nullptr, &owner_->spanPool) != VK_SUCCESS) {
            owner_->spanPool = VK_NULL_HANDLE;
            owner_->timestampNs = -1.0;
            return nullptr;
        }
        for (uint32_t q = kSpanQueries; q >= 2; q -= 2) owner_->freeSpans.push_back(q - 2);
    }
    if (owner_->freeSpans.empty()) return nullptr;
    std::shared_ptr<ComputeSpan> span(new ComputeSpan());
    span->owner_ = owner_;
    span->query_ = owner_->freeSpans.back();
    owner_->freeSpans.pop_back();
    VkCommandBuffer cmd = commands();
    vkCmdResetQueryPool(cmd, owner_->spanPool, span->query_, 2);
    // At the bottom: once everything before it is done, so the span holds
    // only its own work.
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, owner_->spanPool, span->query_);
    return span;
}

void VulkanCompute::endSpan(ComputeSpan& span) {
    if (!context_ || span.owner_ != owner_ || span.ticket_) return;
    VkCommandBuffer cmd = commands();
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, owner_->spanPool, span.query_ + 1);
    span.ticket_ = flush();
}

void VulkanCompute::setTiming(bool on) {
    if (on && timePool_ == VK_NULL_HANDLE && context_) {
        if (!timestamps()) return;
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kTimedQueries;
        if (vkCreateQueryPool(context_->device(), &info, nullptr, &timePool_) != VK_SUCCESS) {
            timePool_ = VK_NULL_HANDLE;
            return;
        }
    }
    timing_ = on && timePool_ != VK_NULL_HANDLE;
}

void VulkanCompute::collectTimes() {
    while (!timed_.empty() && timed_.front().ticket && context_->queue().isComplete(timed_.front().ticket)) {
        const TimedDispatch& t = timed_.front();
        uint64_t stamps[2] = {0, 0};
        if (vkGetQueryPoolResults(context_->device(), timePool_, t.query, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const double ms =
                static_cast<double>((stamps[1] - stamps[0]) & owner_->timestampMask) * owner_->timestampNs * 1e-6;
            auto it = std::find_if(times_.begin(), times_.end(), [&](const ComputeTime& c) { return c.label == *t.label; });
            if (it == times_.end()) it = times_.insert(times_.end(), ComputeTime{*t.label, 0.0, 0});
            it->ms += ms;
            ++it->dispatches;
        }
        timed_.pop_front();
    }
}

std::vector<ComputeTime> VulkanCompute::takeTimes() {
    if (context_) collectTimes();
    return std::exchange(times_, {});
}

bool VulkanCompute::read(const ComputeBuffer& buffer, VkDeviceSize offset, void* out, VkDeviceSize size) {
    if (!context_ || buffer.buffer_ == VK_NULL_HANDLE || offset > buffer.size_ || size > buffer.size_ - offset) return false;
    if (size == 0) return true;
    if (size > readbackSize_) {
        if (readback_ != VK_NULL_HANDLE) context_->destroyBuffer(readback_, readbackAlloc_);
        readback_ = VK_NULL_HANDLE;
        readbackSize_ = 0;
        // Cached host memory where there is some: the CPU reads it back.
        VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (findMemoryType(context_->memoryProperties(), ~0u, props | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) {
            props |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        }
        const VkDeviceSize grown = std::max(size, VkDeviceSize{64 * 1024});
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize memOffset = 0;
        if (!context_->createBuffer(grown, VK_BUFFER_USAGE_TRANSFER_DST_BIT, props, readback_, memory, memOffset,
                                    readbackAlloc_, readbackMapped_)) {
            LOG_ERROR("VulkanCompute: could not allocate a %zu-byte readback buffer", static_cast<size_t>(grown));
            return false;
        }
        readbackSize_ = grown;
    }
    VkCommandBuffer cmd = commands();
    const VkBufferCopy copy{offset, 0, size};
    vkCmdCopyBuffer(cmd, buffer.buffer_, readback_, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_HOST_READ_BIT);
    pendingBarrier_ = true;
    const uint64_t ticket = flush();
    if (!ticket || !context_->queue().wait(ticket)) return false;
    std::memcpy(out, readbackMapped_, static_cast<size_t>(size));
    return true;
}

std::shared_ptr<ComputeReadback> VulkanCompute::readLater(const ComputeBuffer& buffer, VkDeviceSize offset,
                                                          VkDeviceSize size) {
    if (!context_ || buffer.buffer_ == VK_NULL_HANDLE || offset > buffer.size_ || size > buffer.size_ - offset ||
        size == 0)
        return nullptr;
    std::shared_ptr<ComputeReadback> r(new ComputeReadback());
    r->owner_ = owner_;
    VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (findMemoryType(context_->memoryProperties(), ~0u, props | VK_MEMORY_PROPERTY_HOST_CACHED_BIT))
        props |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memOffset = 0;
    if (!context_->createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, props, r->buffer_, memory, memOffset,
                                r->allocId_, r->mapped_)) {
        LOG_ERROR("VulkanCompute: could not allocate a %zu-byte readback buffer", static_cast<size_t>(size));
        return nullptr;
    }
    r->size_ = size;
    owner_->readbacks.insert(r.get());
    VkCommandBuffer cmd = commands();
    const VkBufferCopy copy{offset, 0, size};
    vkCmdCopyBuffer(cmd, buffer.buffer_, r->buffer_, 1, &copy);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                  VK_ACCESS_HOST_READ_BIT);
    pendingBarrier_ = true;
    r->ticket_ = flush();
    if (!r->ticket_) return nullptr;
    return r;
}

uint64_t VulkanCompute::flush() {
    if (open_ == VK_NULL_HANDLE) return lastTicket_;
    // Everything submitted after this sees what the work wrote.
    memoryBarrier(open_, kComputeStages, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    const VkCommandBuffer cmd = open_;
    open_ = VK_NULL_HANDLE;
    pendingBarrier_ = false;
    const uint64_t ticket = context_->frames().submit(cmd);
    if (ticket) lastTicket_ = ticket;
    // The dispatches timed in it finish with it (a failed submission's never).
    for (auto it = timed_.rbegin(); it != timed_.rend() && it->ticket == 0; ++it) it->ticket = ticket;
    if (!ticket)
        while (!timed_.empty() && timed_.back().ticket == 0) timed_.pop_back();
    if (!timed_.empty()) collectTimes();
    return ticket;
}

} // namespace bro::render
