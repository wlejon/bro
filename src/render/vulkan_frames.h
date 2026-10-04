#pragma once

#include "render/vulkan_queue.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
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
    /// be reused, run its deferred destructors and reset its arenas.
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
        uint32_t openCommandBuffers = 0;
        uint64_t ticket = 0;  // retires every queue op of this slot's previous frame
        std::vector<std::function<void()>> deferred;
        std::vector<UploadChunk> uploads;
        std::vector<VkDescriptorPool> descriptorPools;
        size_t activeDescriptorPool = 0;
    };

    void recycle(Slot& slot);
    bool addUploadChunk(Slot& slot, VkDeviceSize minSize);
    void destroyUploadChunk(UploadChunk& chunk);
    VkDescriptorPool createDescriptorPool();

    VulkanContext* context_ = nullptr;
    Slot slots_[kFramesInFlight];
    uint64_t serial_ = 0;
    std::vector<std::function<void()>> pending_;  // deferred during the open frame
    VkDeviceSize minUploadAlignment_ = 16;
};

} // namespace bro::render
