#pragma once

#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <functional>
#include <vector>

namespace bro::scene::vk {

/// Per-frame resources for CPU-GPU synchronization and command submission.
struct SceneVkFrameData {
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence renderFence = VK_NULL_HANDLE;
    VkSemaphore renderSemaphore = VK_NULL_HANDLE;
};

/// Manages graphics command buffers, frame fences, synchronization,
/// and Vulkan 1.3 dynamic rendering function dispatch for 3D scene rendering.
class SceneVkDevice {
public:
    static constexpr uint32_t kMaxFramesInFlight = 2;

    explicit SceneVkDevice(render::VulkanContext& context);
    ~SceneVkDevice();

    SceneVkDevice(const SceneVkDevice&) = delete;
    SceneVkDevice& operator=(const SceneVkDevice&) = delete;

    /// Initialize frame command pools, command buffers, fences, and load dynamic rendering functions.
    bool init();

    /// Tear down per-frame resources and wait for GPU idle.
    void shutdown();

    /// Wait for all GPU work to finish.
    void waitIdle() const;

    /// Wait for previous frame fence, reset command pool, and begin a new frame command buffer.
    VkCommandBuffer beginFrame();

    /// End the current frame command buffer.
    void endFrame();

    /// Submit current frame command buffer to the graphics queue with optional wait/signal semaphores.
    bool submitFrame(VkSemaphore waitSemaphore = VK_NULL_HANDLE,
                     VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VkSemaphore signalSemaphore = VK_NULL_HANDLE);

    /// Synchronously execute a one-shot command buffer on the graphics queue.
    void executeImmediate(const std::function<void(VkCommandBuffer)>& func);

    /// Dynamic rendering dispatchers (Vulkan 1.3 / VK_KHR_dynamic_rendering).
    void cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfoKHR* renderingInfo) const;
    void cmdEndRendering(VkCommandBuffer cmd) const;

    // Timeline semaphore synchronization (if supported)
    bool hasTimelineSemaphore() const { return timelineSemaphore_ != VK_NULL_HANDLE; }
    VkSemaphore timelineSemaphore() const { return timelineSemaphore_; }
    uint64_t currentTimelineValue() const { return timelineValue_; }
    bool waitTimeline(uint64_t value, uint64_t timeoutNs = UINT64_MAX);
    void signalTimeline(uint64_t value);

    // Accessors
    render::VulkanContext& context() { return context_; }
    const render::VulkanContext& context() const { return context_; }
    VkDevice device() const { return context_.device(); }
    VkPhysicalDevice physicalDevice() const { return context_.physicalDevice(); }
    VkQueue graphicsQueue() const { return context_.graphicsQueue(); }
    uint32_t currentFrameIndex() const { return frameIndex_; }
    uint64_t currentFrameNumber() const { return frameNumber_; }
    VkCommandBuffer currentCommandBuffer() const { return frames_[frameIndex_].commandBuffer; }
    const SceneVkFrameData& currentFrameData() const { return frames_[frameIndex_]; }
    bool isInitialized() const { return initialized_; }

private:
    bool initFrameData();
    bool initTimelineSemaphore();
    void loadDynamicRenderingProcs();

    render::VulkanContext& context_;
    bool initialized_ = false;
    uint32_t frameIndex_ = 0;
    uint64_t frameNumber_ = 0;
    bool frameActive_ = false;

    SceneVkFrameData frames_[kMaxFramesInFlight];

    // Immediate submission resources
    VkCommandPool immediateCommandPool_ = VK_NULL_HANDLE;
    VkFence immediateFence_ = VK_NULL_HANDLE;

    // Timeline synchronization
    VkSemaphore timelineSemaphore_ = VK_NULL_HANDLE;
    uint64_t timelineValue_ = 0;

    // Dynamic rendering entry points
    PFN_vkCmdBeginRenderingKHR pfnCmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR pfnCmdEndRendering_ = nullptr;
};

} // namespace bro::scene::vk
