#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>

namespace bro::render {

class VulkanContext;
class VulkanCompute;
struct ComputeOwner;

/// A device-local storage buffer that compute shaders reach by its device
/// address (VulkanComputeFeatures::bufferDeviceAddress): no descriptor sets.
/// Made by VulkanCompute::createBuffer; it reads zero until written. Also
/// usable as a vertex, index or indirect buffer and as a transfer source or
/// destination. Released once the GPU is done with it.
class ComputeBuffer {
public:
    ~ComputeBuffer();
    ComputeBuffer(const ComputeBuffer&) = delete;
    ComputeBuffer& operator=(const ComputeBuffer&) = delete;

    VkBuffer buffer() const { return buffer_; }
    uint64_t address() const { return address_; }
    /// The bytes asked for.
    VkDeviceSize size() const { return size_; }
    /// The bytes behind address(): size() rounded up to 256, at least 256
    /// (an empty buffer still has room for a shader's discarded access).
    VkDeviceSize capacity() const { return capacity_; }

private:
    friend class VulkanCompute;
    ComputeBuffer() = default;
    void release();

    std::shared_ptr<ComputeOwner> owner_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    uint64_t allocId_ = 0;
    uint64_t address_ = 0;
    VkDeviceSize size_ = 0;
    VkDeviceSize capacity_ = 0;
};

/// What a ComputePipeline is made from: one SPIR-V compute entry point whose
/// only resources are a push-constant block (buffers are passed by address).
struct ComputePipelineDesc {
    std::span<const uint32_t> spirv;
    std::string entry = "main";
    uint32_t pushConstantBytes = 0;
    uint32_t localSize[3] = {1, 1, 1};
    /// The local size is given as specialization constants 0..2 (else it is
    /// the module's own LocalSize).
    bool localSizeSpecConstants = false;
};

/// A compute pipeline with a push-constant-only layout. Released once the
/// GPU is done with it.
class ComputePipeline {
public:
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    uint32_t pushConstantBytes() const { return pushBytes_; }

private:
    friend class VulkanCompute;
    ComputePipeline() = default;
    void release();

    std::shared_ptr<ComputeOwner> owner_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint32_t pushBytes_ = 0;
};

/// Compute work on the graphics queue, in submission order with the frames'
/// draws: buffer writes, dispatches and readbacks recorded into one open
/// command buffer of the current frame, which is submitted when something
/// needs its results: a readback, a consumer about to submit work that reads
/// a compute buffer (flush()), or the frame's end. Each recorded operation
/// sees every earlier one's writes; work submitted after a flush sees them
/// all (vertex, index, indirect, shader and transfer reads alike).
///
/// Owned by VulkanContext (compute()). Main thread.
/// Bytes of a buffer copied into host memory without waiting: the copy is
/// recorded after everything recorded before it and submitted at once, and
/// the bytes are there once ready() (the GPU has passed it). Made by
/// VulkanCompute::readLater.
class ComputeReadback {
public:
    ~ComputeReadback();
    ComputeReadback(const ComputeReadback&) = delete;
    ComputeReadback& operator=(const ComputeReadback&) = delete;

    bool ready() const;
    /// The copied bytes once ready(); size() of them.
    const void* data() const { return mapped_; }
    VkDeviceSize size() const { return size_; }

private:
    friend class VulkanCompute;
    ComputeReadback() = default;
    void release();

    std::shared_ptr<ComputeOwner> owner_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    uint64_t allocId_ = 0;
    void* mapped_ = nullptr;
    VkDeviceSize size_ = 0;
    uint64_t ticket_ = 0;
};

class VulkanCompute {
public:
    VulkanCompute() = default;
    ~VulkanCompute();
    VulkanCompute(const VulkanCompute&) = delete;
    VulkanCompute& operator=(const VulkanCompute&) = delete;

    bool init(VulkanContext& context);
    /// Releases every buffer and pipeline still alive (the device is idle);
    /// a handle that outlives it holds nothing.
    void shutdown();

    /// The device reaches buffers by address and computes in 64-bit integers:
    /// what every buffer and pipeline here needs.
    bool available() const;

    std::shared_ptr<ComputeBuffer> createBuffer(VkDeviceSize size, std::string* error = nullptr);
    std::shared_ptr<ComputePipeline> createPipeline(const ComputePipelineDesc& desc, std::string* error = nullptr);

    /// Copy `size` bytes from `data` (now) to `buffer` at `offset`.
    bool write(ComputeBuffer& buffer, VkDeviceSize offset, const void* data, VkDeviceSize size);
    /// Dispatch `groups` workgroups of `pipeline` with `push` as its
    /// push-constant block (pushConstantBytes() long).
    bool dispatch(const ComputePipeline& pipeline, std::span<const uint8_t> push, const uint32_t groups[3]);
    /// Copy `size` bytes of `buffer` at `offset` into `out`, after every
    /// operation recorded so far: submits them and waits for that ticket.
    bool read(const ComputeBuffer& buffer, VkDeviceSize offset, void* out, VkDeviceSize size);
    /// The same copy, not waited for: null on failure.
    std::shared_ptr<ComputeReadback> readLater(const ComputeBuffer& buffer, VkDeviceSize offset, VkDeviceSize size);

    /// Submit what is recorded. Returns its ticket on the graphics queue, or
    /// the last submission's when nothing was recorded since.
    uint64_t flush();

private:
    VkCommandBuffer commands();
    void barrier();

    VulkanContext* context_ = nullptr;
    std::shared_ptr<ComputeOwner> owner_;
    uint64_t frameHook_ = 0;
    VkCommandBuffer open_ = VK_NULL_HANDLE;
    bool pendingBarrier_ = false;
    uint64_t lastTicket_ = 0;

    // Host-visible destination of readbacks, grown as needed.
    VkBuffer readback_ = VK_NULL_HANDLE;
    uint64_t readbackAlloc_ = 0;
    void* readbackMapped_ = nullptr;
    VkDeviceSize readbackSize_ = 0;
};

/// What a buffer or pipeline was made by: the context while it lives, and
/// the handles still alive, which shutdown releases.
struct ComputeOwner {
    VulkanContext* context = nullptr;
    std::unordered_set<ComputeBuffer*> buffers;
    std::unordered_set<ComputePipeline*> pipelines;
    std::unordered_set<ComputeReadback*> readbacks;
};

} // namespace bro::render
