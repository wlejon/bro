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
    }

    // Immediate submission resources
    if (vkCreateCommandPool(dev, &poolInfo, nullptr, &immediateCommandPool_) != VK_SUCCESS) {
        LOG_ERROR("SceneVkDevice: Failed to create immediate command pool");
        return false;
    }

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

    if (immediateCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(dev, immediateCommandPool_, nullptr);
        immediateCommandPool_ = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
        frames_[i].ticket = 0;
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
    if (context_.isValid()) context_.waitIdle();
}

VkCommandBuffer SceneVkDevice::beginFrame() {
    assert(initialized_);
    assert(!frameActive_);

    VkDevice dev = context_.device();
    SceneVkFrameData& frame = frames_[frameIndex_];

    // Wait for the prior execution of this in-flight frame slot
    context_.queue().wait(frame.ticket);

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

    render::QueueSubmit batch;
    batch.commandBuffers.push_back(frame.commandBuffer);
    if (waitSemaphore != VK_NULL_HANDLE) batch.waits.push_back({waitSemaphore, waitStage, 0});
    if (signalSemaphore != VK_NULL_HANDLE) batch.signals.push_back({signalSemaphore, 0});

    const uint64_t ticket = context_.queue().submit(batch);
    if (ticket == 0) {
        LOG_ERROR("SceneVkDevice: Failed to submit command buffer");
        return false;
    }
    frame.ticket = ticket;

    frameIndex_ = (frameIndex_ + 1) % kMaxFramesInFlight;
    frameNumber_++;
    return true;
}

void SceneVkDevice::executeImmediate(const std::function<void(VkCommandBuffer)>& func) {
    assert(initialized_);
    VkDevice dev = context_.device();

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

    render::QueueSubmit batch;
    batch.commandBuffers.push_back(cmd);
    const uint64_t ticket = context_.queue().submit(batch);
    if (ticket != 0) context_.queue().wait(ticket);

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

} // namespace bro::scene::vk
