#pragma once

#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
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

/// The single owner of a device queue: the graphics (and present) queue, and
/// the context's others (VulkanContext::uploadQueue() for the transfer family,
/// asyncComputeQueue()), each with its own timeline and tickets.
///
/// Every vkQueueSubmit and vkQueuePresentKHR in bro goes through here. Each
/// submission signals the queue's timeline semaphore with the next value of a
/// monotonically increasing counter, its ticket. A semaphore signal's first
/// scope is every command submitted to the queue before it, so "ticket N
/// completed" means everything submitted up to and including submission N has
/// finished — a fence for all prior work without a queue- or device-wide wait.
///
/// The queue's own thread makes the driver calls. submit(), submitForeign()
/// and present() take a ticket (or a place in line) under a short lock and
/// return; the thread submits and presents in that order. So a caller never
/// waits for another thread's vkQueueSubmit — which can block in the kernel on
/// a buffer another process (the compositor) holds — and everything that
/// orders by the queue still does: binary semaphores are signalled before they
/// are waited on (acquire, then the submit that waits on it, then the present
/// that waits on the submit's), and a ticket can be waited for (or waited on
/// by another submission) as soon as it is returned, since timeline waits may
/// precede the signal's submission. A batch that waits on another
/// VulkanQueue's timeline is held until that queue has submitted the value.
/// The queues' threads take their driver calls in turn, never two at once
/// (concurrent submits to the graphics and async compute queues hung NVIDIA's
/// Windows driver).
class VulkanQueue {
public:
    VulkanQueue() = default;
    ~VulkanQueue();

    VulkanQueue(const VulkanQueue&) = delete;
    VulkanQueue& operator=(const VulkanQueue&) = delete;

    bool init(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue, uint32_t graphicsFamily);
    void shutdown();

    /// Queue one batch for submission. Returns its ticket, or 0 if the device
    /// is lost (the batch is then dropped). A submission that fails on the
    /// queue's thread is logged and its ticket signalled all the same, so
    /// waits on it end.
    uint64_t submit(const QueueSubmit& batch);

    /// Queue batches recorded by code that drives the queue itself (Skia's
    /// Vulkan backend calls vkQueueSubmit through here): the batches are
    /// copied, and the queue's timeline signal appended to the last so the
    /// work gets a ticket like any other. `fence` signals as with
    /// vkQueueSubmit. Returns VK_SUCCESS, or VK_ERROR_DEVICE_LOST.
    VkResult submitForeign(uint32_t count, const VkSubmitInfo* batches, VkFence fence);

    /// Queue a present after everything queued so far. `onResult` (may be
    /// empty) gets vkQueuePresentKHR's result, on the queue's thread. The
    /// swapchains must not be used on the host (acquire, recreate) until
    /// waitPresented() returns. Returns VK_SUCCESS, or VK_ERROR_DEVICE_LOST.
    VkResult present(const VkPresentInfoKHR& info, std::function<void(VkResult)> onResult = {});
    /// Whether a foreign submission's `fence` is still queued (not yet past
    /// its vkQueueSubmit on the queue's thread): it is unsignalled, and the
    /// host must not touch it (vkQueueSubmit needs the fence to itself).
    bool fenceQueued(VkFence fence) const;
    /// Block until the submission carrying `fence`, if one is queued, is made.
    void waitFenceSubmitted(VkFence fence);
    /// Block until every present queued so far has been made.
    void waitPresented();
    /// Block until the queue's thread has made every call queued so far.
    void drain();

    /// The highest ticket the GPU has finished.
    uint64_t completedTicket() const;
    /// The ticket of the most recent submission (0 before the first).
    uint64_t lastSubmittedTicket() const;
    /// The ticket whose completion implies every queue operation so far has
    /// completed — presents included: after a present that is the *next*
    /// submission's ticket (a later batch is what orders after a present).
    uint64_t retireTicket() const;
    /// retireTicket(), made real now: after a present an empty submission
    /// takes the ticket, so it completes as soon as the work so far has, not
    /// with whatever is submitted next. For a ticket kept to wait on later.
    uint64_t sealTicket();
    bool isComplete(uint64_t ticket) const { return isDeviceLost() || ticket <= completedTicket(); }
    bool isDeviceLost() const { return deviceLost_.load(std::memory_order_relaxed); }

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
    /// One call for the queue's thread: a submission (its own arrays, the
    /// last batch carrying the timeline signal) or a present.
    struct Op {
        enum class Kind { Submit, Present };
        struct Batch {
            std::vector<VkSemaphore> waitSems;
            std::vector<VkPipelineStageFlags> waitStages;
            std::vector<uint64_t> waitValues;  // one per wait (0 for a binary semaphore)
            std::vector<VkCommandBuffer> commandBuffers;
            std::vector<VkSemaphore> signalSems;
            std::vector<uint64_t> signalValues;  // one per signal
            bool protectedSubmit = false;
        };
        Kind kind = Kind::Submit;
        uint64_t ticket = 0;
        std::vector<Batch> batches;
        VkFence fence = VK_NULL_HANDLE;
        std::vector<VkSemaphore> presentWaits;
        std::vector<VkSwapchainKHR> swapchains;
        std::vector<uint32_t> imageIndices;
        std::function<void(VkResult)> onPresent;
    };

    void run();
    void execute(Op& op);
    VkResult submitOp(Op& op);
    void signalAnyway(uint64_t ticket);
    void holdForOtherQueues(const Op& op);
    static VulkanQueue* queueOfTimeline(VkSemaphore timeline);
    /// Block until this queue's thread has submitted `ticket`.
    void waitSubmitted(uint64_t ticket);
    /// Hand `op` to the thread; a submission's ticket (0 if refused).
    uint64_t enqueue(Op&& op);
    void noteFailure(const char* where, VkResult res);

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkQueue presentQueue_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    std::atomic<bool> deviceLost_{false};

    mutable std::mutex mutex_;
    std::condition_variable workCv_;   // the thread: something queued, or stop
    std::condition_variable doneCv_;   // waiters: the thread made a call
    std::deque<Op> pending_;           // guarded by mutex_
    std::thread thread_;
    bool stop_ = false;                // guarded
    bool running_ = false;             // the thread takes calls (guarded)
    uint64_t lastTicket_ = 0;          // the last ticket handed out (guarded)
    bool presentedSince_ = false;      // a present followed lastTicket_ (guarded)
    uint64_t queuedOps_ = 0;           // calls queued (guarded)
    uint64_t doneOps_ = 0;             // calls the thread made (guarded)
    uint64_t submittedTicket_ = 0;     // highest ticket the thread submitted (guarded)
    uint64_t presentsQueued_ = 0;      // guarded
    uint64_t presentsDone_ = 0;        // guarded
    std::vector<VkFence> queuedFences_;  // foreign fences not yet submitted (guarded)

    std::mutex immediateMutex_;
    VkCommandPool immediatePool_ = VK_NULL_HANDLE;
    VkCommandBuffer immediateCmd_ = VK_NULL_HANDLE;
};

} // namespace bro::render
