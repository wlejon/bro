#pragma once

#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <algorithm>
#include <cstdint>
#include <functional>

namespace bro::scene::vk {

/// The 3D scene's view of the shared GPU frame core (VulkanFrames): frame
/// command buffers, per-frame upload memory and descriptor sets, deferred
/// destruction, and an upload stream.
///
/// Uploads (staging copies, mip generation, first layout transitions of new
/// images) are recorded into one upload command buffer per frame. It is
/// submitted ahead of the scene's frame command buffer, and at frame end if
/// nothing rendered, so every upload completes before any command that reads
/// it; the barriers that open and close it order it against GPU work on
/// either side. Nothing here waits, except waitIdle() at teardown.
///
/// A frame may also have a compute stream (computeCommands): work recorded
/// there runs on the async compute queue when the device has one, else it is
/// the frame's own command buffer, so the same recording goes into the
/// graphics stream in the same order. The frame's graphics is split at its
/// first consumer of the compute results (joinCompute): what comes before
/// runs beside the compute, what comes after waits for it on the compute
/// queue's timeline. submitFrame submits the uploads, then the compute, then
/// the graphics: nothing waits on the CPU.
class SceneVkDevice {
public:
    explicit SceneVkDevice(render::VulkanContext& context);
    ~SceneVkDevice();

    SceneVkDevice(const SceneVkDevice&) = delete;
    SceneVkDevice& operator=(const SceneVkDevice&) = delete;

    bool init();
    void shutdown();

    /// Wait for everything submitted through the queue owner so far
    /// (teardown and rebuilds of the whole renderer only).
    void waitIdle() const;

    /// A begun command buffer from the current frame for the scene's passes.
    VkCommandBuffer beginFrame();
    /// Submit the pending uploads, then the compute stream, then the frame's
    /// graphics (the part before its join, then `cmd`). Returns false on
    /// failure.
    bool submitFrame(VkCommandBuffer cmd);
    /// The ticket of the last frame submission (0 before the first).
    uint64_t lastFrameTicket() const { return lastFrameTicket_; }

    /// Compute work has a queue of its own (VulkanContext::asyncComputeQueue).
    bool asyncCompute() { return context_.asyncComputeQueue() != nullptr; }
    /// The queue the compute stream goes to (the graphics queue without an
    /// async one): what its tickets are tickets of.
    render::VulkanQueue& computeQueue();
    /// This frame's compute stream, begun on the first call: a command buffer
    /// on the compute family, submitted ahead of the frame's graphics; without
    /// an async queue, `frameCmd` itself.
    VkCommandBuffer computeCommands(VkCommandBuffer frameCmd);
    /// The compute stream reads what the graphics queue wrote (an upload this
    /// frame or an earlier one): its submission waits for the graphics
    /// queue's work so far. That orders it behind the previous frame's
    /// graphics, so only for a frame whose inputs changed.
    void computeReadsGraphics() { computeAfterGraphics_ = true; }
    /// The compute stream reads what graphics submission `ticket` (an
    /// earlier frame's) wrote: its submission waits for that ticket alone.
    void computeWaitsFor(uint64_t ticket) { computeAfterTicket_ = std::max(computeAfterTicket_, ticket); }
    /// The frame's graphics reaches its first consumer of the compute
    /// results: `cmd`, left open, will be submitted ahead without a wait, and
    /// the returned command buffer (the rest of the frame) waits for the
    /// compute. `cmd` itself when there is no separate compute stream.
    VkCommandBuffer joinCompute(VkCommandBuffer cmd);
    /// The ticket (on computeQueue()) of the last frame's compute, 0 for none.
    uint64_t lastComputeTicket() const { return lastComputeTicket_; }

    /// This frame's upload command buffer, begun on first use.
    VkCommandBuffer uploadCommands();
    /// Submit the upload command buffer if anything was recorded.
    void flushUploads();

    /// `size` bytes of this frame's mapped upload memory (uniforms, staging,
    /// per-frame vertex data), valid until the frame slot is reused.
    render::UploadSlice frameUpload(VkDeviceSize size, VkDeviceSize alignment = 16);
    /// A copy of `data` in this frame's upload memory, aligned for use as a
    /// uniform buffer.
    VkDescriptorBufferInfo frameUniform(const void* data, VkDeviceSize size);
    /// A descriptor set of `layout` valid for this frame only.
    VkDescriptorSet frameSet(VkDescriptorSetLayout layout);
    /// Run `destroy` once the GPU is done with everything submitted so far.
    void defer(std::function<void()> destroy);

    /// Dynamic rendering (Vulkan 1.3 core).
    void cmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* renderingInfo) const;
    void cmdEndRendering(VkCommandBuffer cmd) const;

    render::VulkanContext& context() { return context_; }
    const render::VulkanContext& context() const { return context_; }
    render::VulkanFrames& frames() { return context_.frames(); }
    VkDevice device() const { return context_.device(); }
    VkPhysicalDevice physicalDevice() const { return context_.physicalDevice(); }
    bool isInitialized() const { return initialized_; }

    /// The shadow-map comparison sampler (hardware PCF, LESS_OR_EQUAL, white
    /// border), for the lighting layout's immutable shadow binding. A
    /// comparison sampler cannot be written into a descriptor where
    /// mutableComparisonSamplers is false (MoltenVK's portability subset), so
    /// it lives in the set layout instead, which every device accepts.
    VkSampler shadowCompareSampler() const { return shadowCompareSampler_; }

private:
    render::VulkanContext& context_;
    bool initialized_ = false;
    VkSampler shadowCompareSampler_ = VK_NULL_HANDLE;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    uint64_t lastFrameTicket_ = 0;
    VkCommandBuffer computeCmd_ = VK_NULL_HANDLE;   // this frame's compute stream (async)
    VkCommandBuffer headCmd_ = VK_NULL_HANDLE;      // the graphics before the join
    bool computeAfterGraphics_ = false;
    uint64_t computeAfterTicket_ = 0;
    uint64_t lastComputeTicket_ = 0;
    uint64_t lastFrameSerial_ = 0;   // the frame-ring serial of the last submitFrame
    render::VulkanFrames::HookId frameEndHook_ = 0;
};

} // namespace bro::scene::vk
