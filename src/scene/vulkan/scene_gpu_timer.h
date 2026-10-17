#pragma once
// GPU time of the scene's renders, from timestamp queries written at the start
// and end of each render's command buffer: what perf.gpuFrameMs() reports.
// One set of queries per frame slot, so a set is reset only once the frame
// ring has waited for its previous use. Nothing waits until a caller asks.
//
// With BRO_SCENE_GPU_PASSES=<ms> set, each pass of the frame graph also
// writes a stamp as it ends (mark()), and a render that took longer than
// <ms> logs its time pass by pass ("0" logs every render): where a slow
// frame's time went, without a profiler attached.
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
    /// Reset this frame's queries and write the start stamp (outside rendering).
    void begin(VkCommandBuffer cmd);
    /// The end of the part of the render called `name` (a string that
    /// outlives the timer); a no-op unless passes are being timed.
    void mark(VkCommandBuffer cmd, const char* name);
    /// Write the end stamp; the render is timed once `ticket` completes.
    void end(VkCommandBuffer cmd);
    void submitted(uint64_t ticket);
    /// Milliseconds of the last submitted render, waiting for it (its own
    /// ticket); -1 when nothing was timed since the last call.
    double takeMs();

private:
    static constexpr uint32_t kMaxMarks = 62;
    static constexpr uint32_t kPerSlot = 2 + kMaxMarks;   // start, end, the marks

    render::VulkanContext& context_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double periodNs_ = 1.0;
    uint64_t validMask_ = ~0ull;
    double passLogMs_ = -1.0;               // BRO_SCENE_GPU_PASSES (< 0: passes untimed)
    uint32_t recordingPair_ = UINT32_MAX;   // slot begun in the open command buffer
    uint32_t pendingPair_ = UINT32_MAX;     // slot of the last submitted render
    uint64_t pendingTicket_ = 0;
    // Each slot's marks: how many, and their names.
    uint32_t markCount_[8] = {};
    const char* markNames_[8][kMaxMarks] = {};
};

}  // namespace bro::scene::vk
