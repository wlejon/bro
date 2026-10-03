#include "scene/vulkan/scene_vk_device.h"
#include "util/log.h"

#include <cassert>
#include <cstring>

namespace bro::scene::vk {

SceneVkDevice::SceneVkDevice(render::VulkanContext& context)
    : context_(context)
{
}

SceneVkDevice::~SceneVkDevice() {
    shutdown();
}

bool SceneVkDevice::init() {
    if (initialized_) return true;
    if (!context_.isValid()) {
        LOG_ERROR("SceneVkDevice: VulkanContext is not valid");
        return false;
    }

    if (!initFrameData()) {
        LOG_ERROR("SceneVkDevice: Failed to initialize frame resources");
        shutdown();
        return false;
    }

    initTimelineSemaphore();
    loadDynamicRenderingProcs();

    initialized_ = true;
    return true;
}

bool SceneVkDevice::initFrameData() {
    VkDevice dev = context_.device();
    uint32_t queueFamily = context_.queueFamilies().graphicsFamily;

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (vkCreateCommandPool(dev, &poolInfo, nullptr, &frames_[i].commandPool) != VK_SUCCESS) {
            LOG_ERROR("SceneVkDevice: Failed to create command pool for frame %u", i);
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = frames_[i].commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(dev, &allocInfo, &frames_[i].commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("SceneVkDevice: Failed to allocate command buffer for frame %u", i);
            return false;
        }

        if (vkCreateFence(dev, &fenceInfo, nullptr, &frames_[i].renderFence) != VK_SUCCESS) {
            LOG_ERROR("SceneVkDevice: Failed to create render fence for frame %u", i);
            return false;
        }

        if (vkCreateSemaphore(dev, &semInfo, nullptr, &frames_[i].renderSemaphore) != VK_SUCCESS) {
            LOG_ERROR("SceneVkDevice: Failed to create render semaphore for frame %u", i);
            return false;
        }
    }

    // Immediate submission resources
    if (vkCreateCommandPool(dev, &poolInfo, nullptr, &immediateCommandPool_) != VK_SUCCESS) {
        LOG_ERROR("SceneVkDevice: Failed to create immediate command pool");
        return false;
    }

    VkFenceCreateInfo immFenceInfo{};
    immFenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(dev, &immFenceInfo, nullptr, &immediateFence_) != VK_SUCCESS) {
        LOG_ERROR("SceneVkDevice: Failed to create immediate fence");
        return false;
    }

    return true;
}

bool SceneVkDevice::initTimelineSemaphore() {
    VkSemaphoreTypeCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &timelineInfo;

    VkResult res = vkCreateSemaphore(context_.device(), &semInfo, nullptr, &timelineSemaphore_);
    if (res != VK_SUCCESS) {
        LOG_WARN("SceneVkDevice: Timeline semaphores not available, proceeding without timeline sync");
        timelineSemaphore_ = VK_NULL_HANDLE;
        return false;
    }
    timelineValue_ = 0;
    return true;
}

void SceneVkDevice::loadDynamicRenderingProcs() {
    VkDevice dev = context_.device();

    pfnCmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(dev, "vkCmdBeginRendering"));
    if (!pfnCmdBeginRendering_) {
        pfnCmdBeginRendering_ = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
            vkGetDeviceProcAddr(dev, "vkCmdBeginRenderingKHR"));
    }

    pfnCmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(dev, "vkCmdEndRendering"));
    if (!pfnCmdEndRendering_) {
        pfnCmdEndRendering_ = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
            vkGetDeviceProcAddr(dev, "vkCmdEndRenderingKHR"));
    }

    if (!pfnCmdBeginRendering_ || !pfnCmdEndRendering_) {
        LOG_WARN("SceneVkDevice: Dynamic rendering function pointers could not be resolved");
    }
}

void SceneVkDevice::shutdown() {
    if (!context_.isValid()) return;
    waitIdle();

    VkDevice dev = context_.device();

    if (timelineSemaphore_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(dev, timelineSemaphore_, nullptr);
        timelineSemaphore_ = VK_NULL_HANDLE;
    }

    if (immediateFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(dev, immediateFence_, nullptr);
        immediateFence_ = VK_NULL_HANDLE;
    }

    if (immediateCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, immediateCommandPool_, nullptr);
        immediateCommandPool_ = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        if (frames_[i].renderSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, frames_[i].renderSemaphore, nullptr);
            frames_[i].renderSemaphore = VK_NULL_HANDLE;
        }
        if (frames_[i].renderFence != VK_NULL_HANDLE) {
            vkDestroyFence(dev, frames_[i].renderFence, nullptr);
            frames_[i].renderFence = VK_NULL_HANDLE;
        }
        if (frames_[i].commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(dev, frames_[i].commandPool, nullptr);
            frames_[i].commandPool = VK_NULL_HANDLE;
        }
        frames_[i].commandBuffer = VK_NULL_HANDLE;
    }

    initialized_ = false;
    frameActive_ = false;
}

void SceneVkDevice::waitIdle() const {
    if (context_.isValid() && context_.device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context_.device());
    }
}

VkCommandBuffer SceneVkDevice::beginFrame() {
    assert(initialized_);
    assert(!frameActive_);

    VkDevice dev = context_.device();
    SceneVkFrameData& frame = frames_[frameIndex_];

    // Wait for the prior execution of this in-flight frame slot
    vkWaitForFences(dev, 1, &frame.renderFence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &frame.renderFence);

    vkResetCommandPool(dev, frame.commandPool, 0);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    VkResult res = vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
    assert(res == VK_SUCCESS);
    (void)res;

    frameActive_ = true;
    return frame.commandBuffer;
}

void SceneVkDevice::endFrame() {
    assert(frameActive_);
    VkResult res = vkEndCommandBuffer(frames_[frameIndex_].commandBuffer);
    assert(res == VK_SUCCESS);
    (void)res;
    frameActive_ = false;
}

bool SceneVkDevice::submitFrame(VkSemaphore waitSemaphore,
                               VkPipelineStageFlags waitStage,
                               VkSemaphore signalSemaphore) {
    assert(!frameActive_);
    SceneVkFrameData& frame = frames_[frameIndex_];

    std::vector<VkSemaphore> waitSemaphores;
    std::vector<VkPipelineStageFlags> waitStages;
    if (waitSemaphore != VK_NULL_HANDLE) {
        waitSemaphores.push_back(waitSemaphore);
        waitStages.push_back(waitStage);
    }

    std::vector<VkSemaphore> signalSemaphores;
    if (signalSemaphore != VK_NULL_HANDLE) {
        signalSemaphores.push_back(signalSemaphore);
    }
    // Also signal frame's render semaphore for consumers
    signalSemaphores.push_back(frame.renderSemaphore);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    submitInfo.pWaitSemaphores = waitSemaphores.empty() ? nullptr : waitSemaphores.data();
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer;
    submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
    submitInfo.pSignalSemaphores = signalSemaphores.data();

    // Timeline synchronization if available
    VkTimelineSemaphoreSubmitInfo timelineInfo{};
    uint64_t waitVal = timelineValue_;
    uint64_t signalVal = timelineValue_ + 1;
    if (timelineSemaphore_ != VK_NULL_HANDLE) {
        timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timelineInfo.signalSemaphoreValueCount = 1;
        timelineInfo.pSignalSemaphoreValues = &signalVal;
        submitInfo.pNext = &timelineInfo;
        signalSemaphores.push_back(timelineSemaphore_);
        submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
        submitInfo.pSignalSemaphores = signalSemaphores.data();
    }

    VkResult res = vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, frame.renderFence);
    if (res != VK_SUCCESS) {
        LOG_ERROR("SceneVkDevice: Failed to submit command buffer: %d", res);
        return false;
    }

    if (timelineSemaphore_ != VK_NULL_HANDLE) {
        timelineValue_ = signalVal;
    }

    frameIndex_ = (frameIndex_ + 1) % kMaxFramesInFlight;
    frameNumber_++;
    return true;
}

void SceneVkDevice::executeImmediate(const std::function<void(VkCommandBuffer)>& func) {
    assert(initialized_);
    VkDevice dev = context_.device();

    vkResetFences(dev, 1, &immediateFence_);
    vkResetCommandPool(dev, immediateCommandPool_, 0);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = immediateCommandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    func(cmd);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(context_.graphicsQueue(), 1, &submitInfo, immediateFence_);
    vkWaitForFences(dev, 1, &immediateFence_, VK_TRUE, UINT64_MAX);

    vkFreeCommandBuffers(dev, immediateCommandPool_, 1, &cmd);
}

void SceneVkDevice::cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfoKHR* renderingInfo) const {
    if (pfnCmdBeginRendering_) {
        pfnCmdBeginRendering_(cmd, renderingInfo);
    } else {
        LOG_ERROR("SceneVkDevice: vkCmdBeginRendering not available");
    }
}

void SceneVkDevice::cmdEndRendering(VkCommandBuffer cmd) const {
    if (pfnCmdEndRendering_) {
        pfnCmdEndRendering_(cmd);
    } else {
        LOG_ERROR("SceneVkDevice: vkCmdEndRendering not available");
    }
}

bool SceneVkDevice::waitTimeline(uint64_t value, uint64_t timeoutNs) {
    if (timelineSemaphore_ == VK_NULL_HANDLE) return true;
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timelineSemaphore_;
    waitInfo.pValues = &value;

    return vkWaitSemaphores(context_.device(), &waitInfo, timeoutNs) == VK_SUCCESS;
}

void SceneVkDevice::signalTimeline(uint64_t value) {
    if (timelineSemaphore_ == VK_NULL_HANDLE) return;
    VkSemaphoreSignalInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signalInfo.semaphore = timelineSemaphore_;
    signalInfo.value = value;

    vkSignalSemaphore(context_.device(), &signalInfo);
    timelineValue_ = value;
}

} // namespace bro::scene::vk
