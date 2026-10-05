#pragma once
// GPU time of the scene's renders, from timestamp queries written at the start
// and end of each render's command buffer: what perf.gpuFrameMs() reports.
// One query pair per frame slot, so a pair is reset only once the frame ring
// has waited for its previous use. Nothing waits until a caller asks.
#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <cstdint>

namespace bro::scene::vk {

class SceneGpuTimer {
public:
    explicit SceneGpuTimer(render::VulkanContext& context) : context_(context) {}
    ~SceneGpuTimer();
    SceneGpuTimer(const SceneGpuTimer&) = delete;
    SceneGpuTimer& operator=(const SceneGpuTimer&) = delete;

    /// False when the queue cannot write timestamps (the timer then stays off).
    bool init();
    /// Reset this frame's pair and write the start stamp (outside rendering).
    void begin(VkCommandBuffer cmd);
    /// Write the end stamp; the render is timed once `ticket` completes.
    void end(VkCommandBuffer cmd);
    void submitted(uint64_t ticket);
    /// Milliseconds of the last submitted render, waiting for it (its own
    /// ticket); -1 when nothing was timed since the last call.
    double takeMs();

private:
    render::VulkanContext& context_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double periodNs_ = 1.0;
    uint64_t validMask_ = ~0ull;
    uint32_t recordingPair_ = UINT32_MAX;   // pair begun in the open command buffer
    uint32_t pendingPair_ = UINT32_MAX;     // pair of the last submitted render
    uint64_t pendingTicket_ = 0;
};

}  // namespace bro::scene::vk
