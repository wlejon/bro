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
    uint64_t ticket = 0;  // the VulkanQueue ticket of this slot's last submission
};

/// Manages graphics command buffers, frame pacing, and Vulkan 1.3 dynamic
/// rendering function dispatch for 3D scene rendering. Every submission goes
/// through the context's VulkanQueue; a frame slot is reused once its last
/// submission's ticket has completed.
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

    /// Wait for everything submitted through the queue owner so far.
    void waitIdle() const;

    /// Wait for previous frame fence, reset command pool, and begin a new frame command buffer.
    VkCommandBuffer beginFrame();

    /// End the current frame command buffer.
    void endFrame();

    /// Submit current frame command buffer through the queue owner with optional wait/signal semaphores.
    bool submitFrame(VkSemaphore waitSemaphore = VK_NULL_HANDLE,
                     VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VkSemaphore signalSemaphore = VK_NULL_HANDLE);

    /// Synchronously execute a one-shot command buffer, waiting for its own ticket.
    void executeImmediate(const std::function<void(VkCommandBuffer)>& func);

    /// Dynamic rendering dispatchers (Vulkan 1.3 / VK_KHR_dynamic_rendering).
    void cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfoKHR* renderingInfo) const;
    void cmdEndRendering(VkCommandBuffer cmd) const;

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
    void loadDynamicRenderingProcs();

    render::VulkanContext& context_;
    bool initialized_ = false;
    uint32_t frameIndex_ = 0;
    uint64_t frameNumber_ = 0;
    bool frameActive_ = false;

    SceneVkFrameData frames_[kMaxFramesInFlight];

    // Immediate submission resources
    VkCommandPool immediateCommandPool_ = VK_NULL_HANDLE;

    // Dynamic rendering entry points
    PFN_vkCmdBeginRenderingKHR pfnCmdBeginRendering_ = nullptr;
    PFN_vkCmdEndRenderingKHR pfnCmdEndRendering_ = nullptr;
};

} // namespace bro::scene::vk
