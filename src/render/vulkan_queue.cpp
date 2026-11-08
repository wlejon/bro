#include "render/vulkan_queue.h"
#include "util/log.h"
#include "util/page_pump.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

namespace bro::render {

namespace {

// Every live queue by its timeline, so a batch that waits on another queue's
// timeline can be held until that queue has submitted the value.
std::mutex gQueuesMutex;
std::vector<std::pair<VkSemaphore, VulkanQueue*>> gQueues;

void registerQueue(VkSemaphore timeline, VulkanQueue* queue) {
    std::lock_guard<std::mutex> lock(gQueuesMutex);
    gQueues.emplace_back(timeline, queue);
}

void unregisterQueue(VulkanQueue* queue) {
    std::lock_guard<std::mutex> lock(gQueuesMutex);
    std::erase_if(gQueues, [&](const auto& entry) { return entry.second == queue; });
}

// "bro-vkqueue" in debuggers, profilers and top.
void nameThisThread() {
#if defined(__linux__)
    pthread_setname_np(pthread_self(), "bro-vkqueue");
#elif defined(__APPLE__)
    pthread_setname_np("bro-vkqueue");
#elif defined(_WIN32)
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll"))
        if (auto fn = reinterpret_cast<SetDescription>(
                reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription"))))
            fn(GetCurrentThread(), L"bro-vkqueue");
#endif
}

}  // namespace

VulkanQueue* VulkanQueue::queueOfTimeline(VkSemaphore timeline) {
    std::lock_guard<std::mutex> lock(gQueuesMutex);
    for (const auto& [sem, queue] : gQueues)
        if (sem == timeline) return queue;
    return nullptr;
}

VulkanQueue::~VulkanQueue() {
    shutdown();
}

bool VulkanQueue::init(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue,
                       uint32_t graphicsFamily) {
    device_ = device;
    queue_ = graphicsQueue;
    presentQueue_ = presentQueue ? presentQueue : graphicsQueue;
    deviceLost_.store(false, std::memory_order_relaxed);

    VkSemaphoreTypeCreateInfo typeInfo{};
    typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &typeInfo;
    if (vkCreateSemaphore(device_, &semInfo, nullptr, &timeline_) != VK_SUCCESS) {
        LOG_ERROR("VulkanQueue: failed to create the queue timeline semaphore");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = false;
        running_ = true;
        lastTicket_ = submittedTicket_ = 0;
        presentedSince_ = false;
        queuedOps_ = doneOps_ = presentsQueued_ = presentsDone_ = 0;
        queuedFences_.clear();
    }
    thread_ = std::thread(&VulkanQueue::run, this);
    registerQueue(timeline_, this);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &immediatePool_) != VK_SUCCESS) {
        LOG_ERROR("VulkanQueue: failed to create the immediate command pool");
        return false;
    }
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = immediatePool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocInfo, &immediateCmd_) != VK_SUCCESS) {
        LOG_ERROR("VulkanQueue: failed to allocate the immediate command buffer");
        return false;
    }
    return true;
}

void VulkanQueue::shutdown() {
    if (device_ == VK_NULL_HANDLE) return;
    if (thread_.joinable()) {
        waitIdle();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        workCv_.notify_all();
        thread_.join();  // after making everything still queued
        unregisterQueue(this);
    }
    if (immediatePool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, immediatePool_, nullptr);
        immediatePool_ = VK_NULL_HANDLE;
        immediateCmd_ = VK_NULL_HANDLE;
    }
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
}

void VulkanQueue::noteFailure(const char* where, VkResult res) {
    if (res == VK_ERROR_DEVICE_LOST) {
        if (!deviceLost_.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR("VulkanQueue: device lost (VK_ERROR_DEVICE_LOST) in %s "
                      "(likely the GPU watchdog: a submission ran over ~2 s)",
                      where);
        }
        return;
    }
    LOG_ERROR("VulkanQueue: %s failed (%d)", where, res);
}

uint64_t VulkanQueue::submit(const QueueSubmit& batch) {
    if (isDeviceLost()) return 0;
    Op op;
    Op::Batch b;
    for (const auto& w : batch.waits) {
        b.waitSems.push_back(w.semaphore);
        b.waitStages.push_back(w.stages);
        b.waitValues.push_back(w.value);
    }
    b.commandBuffers = batch.commandBuffers;
    for (const auto& sig : batch.signals) {
        b.signalSems.push_back(sig.semaphore);
        b.signalValues.push_back(sig.value);
    }
    op.batches.push_back(std::move(b));
    return enqueue(std::move(op));
}

VkResult VulkanQueue::submitForeign(uint32_t count, const VkSubmitInfo* batches, VkFence fence) {
    if (isDeviceLost()) return VK_ERROR_DEVICE_LOST;
    // Copied: the caller's arrays and pNext chain are gone once this returns.
    // Skia's batches wait and signal binary semaphores, and chain at most a
    // VkProtectedSubmitInfo.
    Op op;
    op.fence = fence;
    for (uint32_t i = 0; i < count; ++i) {
        const VkSubmitInfo& in = batches[i];
        Op::Batch b;
        b.waitSems.assign(in.pWaitSemaphores, in.pWaitSemaphores + in.waitSemaphoreCount);
        b.waitStages.assign(in.pWaitDstStageMask, in.pWaitDstStageMask + in.waitSemaphoreCount);
        b.waitValues.assign(in.waitSemaphoreCount, 0);
        b.commandBuffers.assign(in.pCommandBuffers, in.pCommandBuffers + in.commandBufferCount);
        b.signalSems.assign(in.pSignalSemaphores, in.pSignalSemaphores + in.signalSemaphoreCount);
        b.signalValues.assign(in.signalSemaphoreCount, 0);
        for (auto* next = static_cast<const VkBaseInStructure*>(in.pNext); next; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO) {
                b.protectedSubmit = reinterpret_cast<const VkProtectedSubmitInfo*>(next)->protectedSubmit;
            } else if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                const auto* t = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(next);
                for (uint32_t k = 0; k < t->waitSemaphoreValueCount && k < in.waitSemaphoreCount; ++k)
                    b.waitValues[k] = t->pWaitSemaphoreValues[k];
                for (uint32_t k = 0; k < t->signalSemaphoreValueCount && k < in.signalSemaphoreCount; ++k)
                    b.signalValues[k] = t->pSignalSemaphoreValues[k];
            } else {
                LOG_ERROR("VulkanQueue: foreign submit chains structure type %d, which is dropped",
                          static_cast<int>(next->sType));
            }
        }
        op.batches.push_back(std::move(b));
    }
    if (op.batches.empty()) op.batches.emplace_back();
    return enqueue(std::move(op)) != 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

VkResult VulkanQueue::present(const VkPresentInfoKHR& info, std::function<void(VkResult)> onResult) {
    if (isDeviceLost()) return VK_ERROR_DEVICE_LOST;
    if (info.pNext) LOG_ERROR("VulkanQueue: a present's pNext chain is not carried to the queue's thread");
    Op op;
    op.kind = Op::Kind::Present;
    op.presentWaits.assign(info.pWaitSemaphores, info.pWaitSemaphores + info.waitSemaphoreCount);
    op.swapchains.assign(info.pSwapchains, info.pSwapchains + info.swapchainCount);
    op.imageIndices.assign(info.pImageIndices, info.pImageIndices + info.swapchainCount);
    op.onPresent = std::move(onResult);
    enqueue(std::move(op));
    return VK_SUCCESS;
}

uint64_t VulkanQueue::enqueue(Op&& op) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (isDeviceLost() || !running_) return 0;
    uint64_t ticket = 0;
    if (op.kind == Op::Kind::Present) {
        presentedSince_ = true;
        ++presentsQueued_;
    } else {
        // The queue's timeline signal rides on the last batch.
        ticket = ++lastTicket_;
        op.ticket = ticket;
        Op::Batch& last = op.batches.back();
        last.signalSems.push_back(timeline_);
        last.signalValues.push_back(ticket);
        presentedSince_ = false;
    }
    ++queuedOps_;
    if (op.fence != VK_NULL_HANDLE) queuedFences_.push_back(op.fence);
    pending_.push_back(std::move(op));
    lock.unlock();
    workCv_.notify_one();
    return ticket;
}

void VulkanQueue::run() {
    nameThisThread();
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        workCv_.wait(lock, [&] { return stop_ || !pending_.empty(); });
        if (pending_.empty()) break;  // stopping, and everything queued is made
        const bool present = pending_.front().kind == Op::Kind::Present;
        uint64_t ticket = 0;
        VkFence fence = VK_NULL_HANDLE;
        {
            Op op = std::move(pending_.front());
            pending_.pop_front();
            ticket = op.ticket;
            fence = op.fence;
            lock.unlock();
            execute(op);
        }
        lock.lock();
        if (fence != VK_NULL_HANDLE) {
            auto it = std::find(queuedFences_.begin(), queuedFences_.end(), fence);
            if (it != queuedFences_.end()) queuedFences_.erase(it);
        }
        if (present) ++presentsDone_;
        else submittedTicket_ = std::max(submittedTicket_, ticket);
        ++doneOps_;
        doneCv_.notify_all();
    }
    running_ = false;
    doneCv_.notify_all();
}

void VulkanQueue::execute(Op& op) {
    if (op.kind == Op::Kind::Present) {
        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = static_cast<uint32_t>(op.presentWaits.size());
        info.pWaitSemaphores = op.presentWaits.data();
        info.swapchainCount = static_cast<uint32_t>(op.swapchains.size());
        info.pSwapchains = op.swapchains.data();
        info.pImageIndices = op.imageIndices.data();
        VkResult res = isDeviceLost() ? VK_ERROR_DEVICE_LOST : vkQueuePresentKHR(presentQueue_, &info);
        if (res == VK_ERROR_DEVICE_LOST) noteFailure("vkQueuePresentKHR", res);
        if (op.onPresent) op.onPresent(res);
        return;
    }
    holdForOtherQueues(op);
    const VkResult res = isDeviceLost() ? VK_ERROR_DEVICE_LOST : submitOp(op);
    if (res == VK_SUCCESS) return;
    noteFailure(op.fence != VK_NULL_HANDLE || op.batches.size() > 1 ? "foreign vkQueueSubmit" : "vkQueueSubmit",
                res);
    signalAnyway(op.ticket);
}

VkResult VulkanQueue::submitOp(Op& op) {
    const size_t n = op.batches.size();
    std::vector<VkSubmitInfo> infos(n);
    std::vector<VkTimelineSemaphoreSubmitInfo> timelines(n);
    std::vector<VkProtectedSubmitInfo> protects(n);
    for (size_t i = 0; i < n; ++i) {
        Op::Batch& b = op.batches[i];
        VkTimelineSemaphoreSubmitInfo& t = timelines[i];
        t.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        t.waitSemaphoreValueCount = static_cast<uint32_t>(b.waitValues.size());
        t.pWaitSemaphoreValues = b.waitValues.data();
        t.signalSemaphoreValueCount = static_cast<uint32_t>(b.signalValues.size());
        t.pSignalSemaphoreValues = b.signalValues.data();
        if (b.protectedSubmit) {
            protects[i].sType = VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO;
            protects[i].protectedSubmit = VK_TRUE;
            t.pNext = &protects[i];
        }
        VkSubmitInfo& info = infos[i];
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.pNext = &t;
        info.waitSemaphoreCount = static_cast<uint32_t>(b.waitSems.size());
        info.pWaitSemaphores = b.waitSems.data();
        info.pWaitDstStageMask = b.waitStages.data();
        info.commandBufferCount = static_cast<uint32_t>(b.commandBuffers.size());
        info.pCommandBuffers = b.commandBuffers.data();
        info.signalSemaphoreCount = static_cast<uint32_t>(b.signalSems.size());
        info.pSignalSemaphores = b.signalSems.data();
    }
    return vkQueueSubmit(queue_, static_cast<uint32_t>(n), infos.data(), op.fence);
}

// A failed submission's ticket still completes, so nothing waits on it for
// good: an empty batch signals it in order; failing that (device loss), the
// host signals it once the ticket before it is done.
void VulkanQueue::signalAnyway(uint64_t ticket) {
    if (!isDeviceLost()) {
        VkTimelineSemaphoreSubmitInfo t{};
        t.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        t.signalSemaphoreValueCount = 1;
        t.pSignalSemaphoreValues = &ticket;
        VkSubmitInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.pNext = &t;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &timeline_;
        if (vkQueueSubmit(queue_, 1, &info, VK_NULL_HANDLE) == VK_SUCCESS) return;
    }
    if (ticket > 1) {
        const uint64_t before = ticket - 1;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &timeline_;
        wait.pValues = &before;
        vkWaitSemaphores(device_, &wait, 2'000'000'000ull);
    }
    VkSemaphoreSignalInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal.semaphore = timeline_;
    signal.value = ticket;
    vkSignalSemaphore(device_, &signal);
}

// A wait on another queue's timeline is submitted only once that queue has
// submitted the value: the driver then never sees a wait before its signal.
void VulkanQueue::holdForOtherQueues(const Op& op) {
    for (const Op::Batch& b : op.batches) {
        for (size_t i = 0; i < b.waitSems.size(); ++i) {
            if (b.waitSems[i] == timeline_ || b.waitValues[i] == 0) continue;
            if (VulkanQueue* other = queueOfTimeline(b.waitSems[i])) other->waitSubmitted(b.waitValues[i]);
        }
    }
}

void VulkanQueue::waitSubmitted(uint64_t ticket) {
    std::unique_lock<std::mutex> lock(mutex_);
    doneCv_.wait(lock, [&] { return submittedTicket_ >= ticket || ticket > lastTicket_ || !running_; });
}

void VulkanQueue::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t target = queuedOps_;
    doneCv_.wait(lock, [&] { return doneOps_ >= target || !running_; });
}

bool VulkanQueue::fenceQueued(VkFence fence) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(queuedFences_.begin(), queuedFences_.end(), fence) != queuedFences_.end();
}

void VulkanQueue::waitFenceSubmitted(VkFence fence) {
    std::unique_lock<std::mutex> lock(mutex_);
    doneCv_.wait(lock, [&] {
        return !running_ || std::find(queuedFences_.begin(), queuedFences_.end(), fence) == queuedFences_.end();
    });
}

void VulkanQueue::waitPresented() {
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t target = presentsQueued_;
    doneCv_.wait(lock, [&] { return presentsDone_ >= target || !running_; });
}

uint64_t VulkanQueue::completedTicket() const {
    uint64_t value = 0;
    if (timeline_ != VK_NULL_HANDLE)
        vkGetSemaphoreCounterValue(device_, timeline_, &value);
    return value;
}

uint64_t VulkanQueue::lastSubmittedTicket() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastTicket_;
}

uint64_t VulkanQueue::retireTicket() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastTicket_ + (presentedSince_ ? 1 : 0);
}

uint64_t VulkanQueue::sealTicket() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!presentedSince_) return lastTicket_;
    }
    const uint64_t ticket = submit(QueueSubmit{});
    return ticket != 0 ? ticket : retireTicket();
}

bool VulkanQueue::wait(uint64_t ticket, uint64_t timeoutNs) {
    if (ticket == 0 || timeline_ == VK_NULL_HANDLE) return true;
    if (isDeviceLost()) return false;
    if (ticket > lastSubmittedTicket() && submit(QueueSubmit{}) < ticket) {
        if (!isDeviceLost()) {
            LOG_ERROR("VulkanQueue: waiting for ticket %llu that will never be submitted",
                      static_cast<unsigned long long>(ticket));
        }
        return false;
    }
    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &timeline_;
    info.pValues = &ticket;
    VkResult res;
    if (timeoutNs == UINT64_MAX && util::pumpsWhileWaiting()) {
        // The page thread: waited in steps, the window's messages let in
        // between them (util/page_pump.h; a GPU wait draws nothing).
        constexpr uint64_t kStepNs = uint64_t{util::kPumpIntervalMs} * 1'000'000;
        while ((res = vkWaitSemaphores(device_, &info, kStepNs)) == VK_TIMEOUT && !isDeviceLost()) {
            util::pumpPageThread(util::PumpReason::GpuWait);
        }
    } else {
        res = vkWaitSemaphores(device_, &info, timeoutNs);
    }
    if (res != VK_SUCCESS && res != VK_TIMEOUT) noteFailure("waiting for ticket", res);
    return res == VK_SUCCESS;
}

bool VulkanQueue::submitImmediate(const std::function<void(VkCommandBuffer)>& record) {
    std::lock_guard<std::mutex> lock(immediateMutex_);
    vkResetCommandPool(device_, immediatePool_, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(immediateCmd_, &begin);
    record(immediateCmd_);
    vkEndCommandBuffer(immediateCmd_);

    QueueSubmit batch;
    batch.commandBuffers.push_back(immediateCmd_);
    const uint64_t ticket = submit(batch);
    return ticket != 0 && wait(ticket);
}

} // namespace bro::render
