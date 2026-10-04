#pragma once

#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
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
    /// Submit the pending uploads, then `cmd`. Returns false on failure.
    bool submitFrame(VkCommandBuffer cmd);
    /// The ticket of the last frame submission (0 before the first).
    uint64_t lastFrameTicket() const { return lastFrameTicket_; }

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

private:
    render::VulkanContext& context_;
    bool initialized_ = false;
    VkCommandBuffer uploadCmd_ = VK_NULL_HANDLE;
    uint64_t lastFrameTicket_ = 0;
    render::VulkanFrames::HookId frameEndHook_ = 0;
};

} // namespace bro::scene::vk
