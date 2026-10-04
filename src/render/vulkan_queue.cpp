#include "render/vulkan_queue.h"
#include "util/log.h"

namespace bro::render {

VulkanQueue::~VulkanQueue() {
    shutdown();
}

bool VulkanQueue::init(VkDevice device, VkQueue graphicsQueue, VkQueue presentQueue,
                       uint32_t graphicsFamily) {
    device_ = device;
    queue_ = graphicsQueue;
    presentQueue_ = presentQueue ? presentQueue : graphicsQueue;

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
    waitIdle();
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

uint64_t VulkanQueue::submit(const QueueSubmit& batch) {
    std::vector<VkSemaphore> waitSems;
    std::vector<VkPipelineStageFlags> waitStages;
    std::vector<uint64_t> waitValues;
    for (const auto& w : batch.waits) {
        waitSems.push_back(w.semaphore);
        waitStages.push_back(w.stages);
        waitValues.push_back(w.value);
    }
    std::vector<VkSemaphore> signalSems;
    std::vector<uint64_t> signalValues;
    for (const auto& s : batch.signals) {
        signalSems.push_back(s.semaphore);
        signalValues.push_back(s.value);
    }
    signalSems.push_back(timeline_);
    signalValues.push_back(0);  // filled under the lock

    std::lock_guard<std::mutex> lock(submitMutex_);
    const uint64_t ticket = lastTicket_ + 1;
    signalValues.back() = ticket;

    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timelineInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
    timelineInfo.pWaitSemaphoreValues = waitValues.data();
    timelineInfo.signalSemaphoreValueCount = static_cast<uint32_t>(signalValues.size());
    timelineInfo.pSignalSemaphoreValues = signalValues.data();

    VkSubmitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    info.pNext = &timelineInfo;
    info.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
    info.pWaitSemaphores = waitSems.data();
    info.pWaitDstStageMask = waitStages.data();
    info.commandBufferCount = static_cast<uint32_t>(batch.commandBuffers.size());
    info.pCommandBuffers = batch.commandBuffers.data();
    info.signalSemaphoreCount = static_cast<uint32_t>(signalSems.size());
    info.pSignalSemaphores = signalSems.data();

    VkResult res = vkQueueSubmit(queue_, 1, &info, VK_NULL_HANDLE);
    if (res != VK_SUCCESS) {
        LOG_ERROR("VulkanQueue: vkQueueSubmit failed (%d)", res);
        return 0;
    }
    lastTicket_ = ticket;
    presentedSince_ = false;
    return ticket;
}

VkResult VulkanQueue::present(const VkPresentInfoKHR& info) {
    std::lock_guard<std::mutex> lock(submitMutex_);
    presentedSince_ = true;
    return vkQueuePresentKHR(presentQueue_, &info);
}

uint64_t VulkanQueue::completedTicket() const {
    uint64_t value = 0;
    if (timeline_ != VK_NULL_HANDLE)
        vkGetSemaphoreCounterValue(device_, timeline_, &value);
    return value;
}

uint64_t VulkanQueue::lastSubmittedTicket() const {
    std::lock_guard<std::mutex> lock(submitMutex_);
    return lastTicket_;
}

uint64_t VulkanQueue::retireTicket() const {
    std::lock_guard<std::mutex> lock(submitMutex_);
    return lastTicket_ + (presentedSince_ ? 1 : 0);
}

bool VulkanQueue::wait(uint64_t ticket, uint64_t timeoutNs) {
    if (ticket == 0 || timeline_ == VK_NULL_HANDLE) return true;
    if (ticket > lastSubmittedTicket() && submit(QueueSubmit{}) < ticket) {
        LOG_ERROR("VulkanQueue: waiting for ticket %llu that will never be submitted",
                  static_cast<unsigned long long>(ticket));
        return false;
    }
    VkSemaphoreWaitInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    info.semaphoreCount = 1;
    info.pSemaphores = &timeline_;
    info.pValues = &ticket;
    VkResult res = vkWaitSemaphores(device_, &info, timeoutNs);
    if (res != VK_SUCCESS && res != VK_TIMEOUT)
        LOG_ERROR("VulkanQueue: waiting for ticket %llu failed (%d)",
                  static_cast<unsigned long long>(ticket), res);
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
