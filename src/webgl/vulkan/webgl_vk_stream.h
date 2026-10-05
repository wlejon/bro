#pragma once

#include "render/vulkan_frames.h"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace bro::render { class VulkanContext; }

namespace bro::webgl::vk {

/// A WebGL context's command stream and the memory its commands use.
///
/// WebGL records every GPU operation, in API order, into one open command
/// buffer and submits it at flushes: readbacks, client waits, gl.flush, the
/// engine's frame end. Unlike the engine's frame ring, the stream does not
/// depend on frames to recycle anything: each submission closes a *segment*
/// (command pool, upload memory, descriptor sets, deferred destructions), and
/// segments rotate through a small ring, each reused once its own ticket has
/// completed. A script that uploads or draws in a loop with no frame boundary
/// therefore runs in bounded memory: once a segment has used its budget the
/// context submits at its next safe point (wantsSubmit), and the ring waits
/// for the oldest segment rather than growing.
///
/// Main thread only.
class WebGLVkStream {
public:
    explicit WebGLVkStream(render::VulkanContext& context);
    ~WebGLVkStream();

    WebGLVkStream(const WebGLVkStream&) = delete;
    WebGLVkStream& operator=(const WebGLVkStream&) = delete;

    /// The open command buffer, begun on first use.
    VkCommandBuffer commands();
    bool hasCommands() const { return cmd_ != VK_NULL_HANDLE; }

    /// Submit the open command buffer (if any) and move on to the next
    /// segment. Returns false if the submission failed.
    bool submit();
    /// The ticket of this stream's latest submission (0 before the first).
    uint64_t lastTicket() const { return lastTicket_; }

    /// True once the open segment has used its memory budget: the caller
    /// should submit at its next point outside a render pass.
    bool wantsSubmit() const;

    /// `size` bytes of mapped, coherent memory usable as a transfer source,
    /// uniform, vertex or index buffer by commands of the open segment.
    render::UploadSlice allocUpload(VkDeviceSize size, VkDeviceSize alignment);

    /// A descriptor set of `layout` for commands of the open segment.
    VkDescriptorSet allocDescriptorSet(VkDescriptorSetLayout layout);

    /// Run `destroy` once the GPU has finished every command recorded so far.
    void defer(std::function<void()> destroy);

    /// Changes whenever a segment closes. Upload slices and descriptor sets
    /// are valid only within the segment they came from, so a cache of them
    /// keys on this.
    uint64_t segmentSerial() const { return serial_; }

private:
    struct UploadChunk {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize memoryOffset = 0;
        uint64_t allocId = 0;
        void* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize used = 0;
    };
    struct Segment {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        std::vector<VkCommandBuffer> commandBuffers;
        size_t nextCommandBuffer = 0;
        std::vector<UploadChunk> uploads;
        VkDeviceSize uploadBytes = 0;
        std::vector<VkDescriptorPool> descriptorPools;
        size_t activeDescriptorPool = 0;
        uint32_t descriptorSets = 0;
        std::vector<std::function<void()>> deferred;
        uint64_t ticket = 0;  // completes every command of the segment
    };
    static constexpr uint32_t kSegments = 3;

    Segment& open() { return segments_[index_]; }
    void recycle(Segment& segment);
    bool addUploadChunk(Segment& segment, VkDeviceSize minSize);
    void destroyUploadChunk(UploadChunk& chunk);
    VkDescriptorPool createDescriptorPool();

    render::VulkanContext& context_;
    std::array<Segment, kSegments> segments_{};
    uint32_t index_ = 0;
    uint64_t serial_ = 1;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    uint64_t lastTicket_ = 0;
    VkDeviceSize minUploadAlignment_ = 16;
};

} // namespace bro::webgl::vk
