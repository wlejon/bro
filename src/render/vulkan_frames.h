#pragma once

#include "render/vulkan_queue.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace bro::render {

class VulkanContext;

/// A slice of a frame's host-visible upload memory.
struct UploadSlice {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void* mapped = nullptr;  // host pointer to `offset`
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

/// The frames-in-flight ring every GPU consumer in bro shares.
///
/// One engine frame = one beginFrame(). Each of the kFramesInFlight slots owns
/// a command pool, a linear host-visible upload arena (uniforms, staging) and
/// a descriptor arena; beginFrame() waits until the slot's previous use has
/// left the GPU — its ticket on the VulkanQueue, not the device — and then
/// recycles all three wholesale. Consumers key their own per-frame ring
/// resources off frameIndex().
///
/// With an async compute queue (VulkanContext::asyncComputeQueue), a slot
/// also owns a command pool on the compute family (beginComputeCommands).
/// Its reuse does not wait for the slot's work on that queue: a slot whose
/// compute work is still running (a background job longer than the frames,
/// bro.kernel.background) retires its compute pool, uploads, descriptor
/// pools and deferred destructors to be released once that work is done,
/// and takes fresh ones, so the frames go on beside it.
///
/// Deferred destruction: defer(fn) runs `fn` once every submission made up to
/// the end of the current frame has completed, so a resource can be released
/// as soon as the CPU is done with it even while the GPU may still read it.
/// Contract: commands that use a deferred resource are submitted within the
/// frame defer() was called in (a frame's command buffers never outlive it).
///
/// Main thread only; the queue it submits through is the thread-safe part.
class VulkanFrames {
public:
    static constexpr uint32_t kFramesInFlight = 2;

    VulkanFrames() = default;
    ~VulkanFrames();

    VulkanFrames(const VulkanFrames&) = delete;
    VulkanFrames& operator=(const VulkanFrames&) = delete;

    bool init(VulkanContext& context);
    /// Waits for the queue, runs every deferred destructor, frees the slots.
    void shutdown();

    /// Start the next frame: close the current one, wait for the slot about to
    /// be reused, run its deferred destructors and reset its arenas. A frame
    /// that took nothing of its slot (no command buffer, upload or descriptor
    /// set) stays open instead, as the frame that follows, while the slot
    /// next is still on the GPU: it holds no GPU
    /// work to wait for, and closing it would spend a slot, so the next
    /// frame's would be the one the GPU is still drawing (a headless
    /// advanceTime of two steps, the first drawing nothing, waited each frame
    /// for the frame before it).
    void beginFrame();
    /// Begin a frame if none has been begun yet (for standalone users).
    void ensureFrame() { if (serial_ == 0) beginFrame(); }

    uint32_t frameIndex() const { return static_cast<uint32_t>(serial_ % kFramesInFlight); }
    /// Monotonic frame number (0 before the first beginFrame).
    uint64_t frameSerial() const { return serial_; }

    /// A primary command buffer from this frame's pool, already begun
    /// (ONE_TIME_SUBMIT). It must be submitted before the frame ends.
    VkCommandBuffer beginCommands();
    /// End `cmd` (from beginCommands) and submit it through the queue.
    /// Returns the ticket, 0 on failure.
    uint64_t submit(VkCommandBuffer cmd, std::vector<SemaphoreWait> waits = {},
                    std::vector<SemaphoreSignal> signals = {});

    /// A primary command buffer from this frame's compute-family pool, begun
    /// (ONE_TIME_SUBMIT), for the async compute queue; null without one.
    VkCommandBuffer beginComputeCommands();
    /// End `cmd` (from beginComputeCommands) and submit it to the async
    /// compute queue. Returns that queue's ticket, 0 on failure.
    uint64_t submitCompute(VkCommandBuffer cmd, std::vector<SemaphoreWait> waits = {},
                           std::vector<SemaphoreSignal> signals = {});

    /// `size` bytes of mapped, host-coherent memory valid until this slot is
    /// reused, aligned to `alignment` (and at least the device's uniform and
    /// copy-offset alignments). Usable as a transfer source, uniform, storage,
    /// vertex or index buffer.
    UploadSlice allocUpload(VkDeviceSize size, VkDeviceSize alignment = 16);

    /// A descriptor set of `layout` valid for this frame; the arena is reset
    /// when the slot is reused, so the set never needs freeing or updating
    /// while in flight.
    VkDescriptorSet allocDescriptorSet(VkDescriptorSetLayout layout);

    /// Run `destroy` once the GPU is done with everything submitted through
    /// the end of the current frame.
    void defer(std::function<void()> destroy);

    /// When no command buffers are open in the current slot and all submitted
    /// GPU work has completed (waiting first when `wait` is true), run all
    /// pending deferred callbacks and recycle the current slot's transient
    /// command buffers, upload chunks, and descriptor pools.
    bool reclaimCompleted(bool wait = false);

    /// Register `hook` to run when the current frame closes (at the next
    /// beginFrame, before the closing frame's ticket is taken). Consumers that
    /// record into frame command buffers across calls — WebGL, the scene's
    /// upload stream — submit their open work here, so no command buffer or
    /// upload slice outlives its frame. Returns an id for removeFrameEndHook.
    using HookId = uint64_t;
    HookId addFrameEndHook(std::function<void()> hook);
    void removeFrameEndHook(HookId id);

    VulkanQueue& queue();

private:
    struct UploadChunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize memoryOffset = 0;
        uint64_t allocId = 0;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    struct Slot {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commandBuffers;
        size_t nextCommandBuffer = 0;
        VkCommandPool computePool = VK_NULL_HANDLE;   // the async compute family's
        std::vector<VkCommandBuffer> computeBuffers;
        size_t nextComputeBuffer = 0;
        uint32_t openCommandBuffers = 0;
        bool used = false;    // a command buffer, upload or descriptor set taken this frame
        uint64_t ticket = 0;  // retires every queue op of this slot's previous frame
        uint64_t computeTicket = 0;   // the same on the async compute queue
        bool computeUsed = false;     // a compute command buffer taken this frame
        std::vector<std::function<void()>> deferred;
        std::vector<UploadChunk> uploads;
        std::vector<VkDescriptorPool> descriptorPools;
        size_t activeDescriptorPool = 0;
    };

    // A slot's part still in use by the async compute queue, released once
    // its ticket there is complete.
    struct Retired {
        uint64_t computeTicket = 0;
        VkCommandPool computePool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> computeBuffers;
        std::vector<std::function<void()>> deferred;
        std::vector<UploadChunk> uploads;
        std::vector<VkDescriptorPool> descriptorPools;
    };

    void recycle(Slot& slot);
    void retire(Slot& slot);
    // Release the retired parts whose compute work is done (all of them
    // when `all`: the queue is known idle).
    void releaseRetired(bool all);
    VkCommandBuffer nextBuffer(VkCommandPool pool, std::vector<VkCommandBuffer>& buffers, size_t& next);
    bool addUploadChunk(Slot& slot, VkDeviceSize minSize);
    void destroyUploadChunk(UploadChunk& chunk);
    VkDescriptorPool createDescriptorPool();

    VulkanContext* context_ = nullptr;
    Slot slots_[kFramesInFlight];
    uint64_t serial_ = 0;
    std::vector<std::function<void()>> pending_;  // deferred during the open frame
    std::vector<Retired> retired_;                // in ticket order
    // Compute pools released from retirement, reset, with their buffers.
    std::vector<std::pair<VkCommandPool, std::vector<VkCommandBuffer>>> spareComputePools_;
    std::vector<std::pair<HookId, std::function<void()>>> frameEndHooks_;
    HookId nextHookId_ = 1;
    VkDeviceSize minUploadAlignment_ = 16;
};

} // namespace bro::render
