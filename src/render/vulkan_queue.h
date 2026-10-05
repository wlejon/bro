#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace bro::render {

/// A semaphore a submission waits on before `stages` run. `value` is the
/// timeline value to reach (ignored for binary semaphores).
struct SemaphoreWait {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    uint64_t value = 0;
};

/// A semaphore a submission signals when it completes.
struct SemaphoreSignal {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    uint64_t value = 0;
};

/// One batch for VulkanQueue::submit.
struct QueueSubmit {
    std::vector<VkCommandBuffer> commandBuffers;
    std::vector<SemaphoreWait> waits;
    std::vector<SemaphoreSignal> signals;
};

/// The single owner of the device's graphics (and present) queue.
///
/// VkQueue needs external synchronisation, so every vkQueueSubmit and
/// vkQueuePresentKHR in bro goes through here, under one lock. Each submission
/// also signals the queue's timeline semaphore with the next value of a
/// monotonically increasing counter and returns it as the submission's ticket.
/// A semaphore signal's first scope is every command submitted to the queue
/// before it, so "ticket N completed" means everything submitted up to and
/// including submission N has finished — a fence for all prior work without a
/// queue- or device-wide wait.
class VulkanQueue {
public:
    VulkanQueue() = default;
    ~VulkanQueue();

    VulkanQueue(const VulkanQueue&) = delete;
    VulkanQueue& operator=(const VulkanQueue&) = delete;

    bool init(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue, uint32_t graphicsFamily);
    void shutdown();

    /// Submit one batch. Returns its ticket, or 0 if the submission failed (the
    /// command buffers were then not executed and no semaphore will signal).
    uint64_t submit(const QueueSubmit& batch);

    /// Submit batches recorded by code that drives the queue itself (Skia's
    /// Vulkan backend calls vkQueueSubmit through here): the same lock, and
    /// the queue's timeline signal appended to the last batch so the work
    /// gets a ticket like any other. Returns the vkQueueSubmit result.
    VkResult submitForeign(uint32_t count, const VkSubmitInfo* batches, VkFence fence);

    /// Present on the present queue (serialised with submissions).
    VkResult present(const VkPresentInfoKHR& info);

    /// The highest ticket the GPU has finished.
    uint64_t completedTicket() const;
    /// The ticket of the most recent submission (0 before the first).
    uint64_t lastSubmittedTicket() const;
    /// The ticket whose completion implies every queue operation so far has
    /// completed — presents included: after a present that is the *next*
    /// submission's ticket (a later batch is what orders after a present).
    uint64_t retireTicket() const;
    bool isComplete(uint64_t ticket) const { return ticket <= completedTicket(); }

    /// Block until `ticket` has completed. A ticket not yet submitted (from
    /// retireTicket) is made real with an empty submission first. Returns
    /// false on timeout or device loss.
    bool wait(uint64_t ticket, uint64_t timeoutNs = UINT64_MAX);

    /// Wait for every queue operation so far: the queue-scoped replacement
    /// for vkQueueWaitIdle / vkDeviceWaitIdle.
    bool waitIdle() { return wait(retireTicket()); }

    /// Record `record` into a command buffer, submit it and wait for it (its
    /// own ticket, not the device). For uploads, readbacks and one-off layout
    /// work outside the frame. Returns false if the submission failed.
    /// Not re-entrant: `record` must not itself call submitImmediate.
    bool submitImmediate(const std::function<void(VkCommandBuffer)>& record);

    /// The timeline semaphore every submission signals, for consumers that
    /// want to wait on a ticket from another submission.
    VkSemaphore timeline() const { return timeline_; }
    VkQueue queue() const { return queue_; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;

    mutable std::mutex submitMutex_;
    uint64_t lastTicket_ = 0;      // guarded by submitMutex_
    bool presentedSince_ = false;  // a present followed lastTicket_ (guarded)

    std::mutex immediateMutex_;
    VkCommandPool immediatePool_ = VK_NULL_HANDLE;
    VkCommandBuffer immediateCmd_ = VK_NULL_HANDLE;
};

} // namespace bro::render
