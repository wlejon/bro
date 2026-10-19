#pragma once
// GPU time of the scene's renders, from timestamp queries written at the start
// and end of each render's command buffer: what perf.gpuFrameMs() reports.
// One set of queries per frame slot, so a set is reset only once the frame
// ring has waited for its previous use. Nothing waits until a caller asks.
//
// With BRO_SCENE_GPU_PASSES=<ms> set, each pass of the frame graph also
// writes a stamp as it ends (mark()), and a render that took longer than
// <ms> logs its time pass by pass ("0" logs every render): where a slow
// frame's time went, without a profiler attached. With BRO_SCENE_GPU_FRAGS
// set as well, each opaque draw's fragment shader invocations follow the line
// (statBegin): its overdraw, where a pass is fragment-bound.
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
    /// ticket); -1 when nothing was timed since the last call. Not `wait`:
    /// the oldest render that has finished and was not taken yet, -1 for
    /// none, so a caller reading every frame reads each render once, a frame
    /// or so behind, and its frames overlap the GPU's.
    double takeMs(bool wait = true);
    /// Count the fragment shader invocations of one draw called `name`
    /// (inside rendering, closed by statEnd); a no-op unless
    /// BRO_SCENE_GPU_FRAGS is set, when a logged render's line is followed
    /// by its draws' counts ("scene FRAG: name count ...").
    void statBegin(VkCommandBuffer cmd, const char* name);
    void statEnd(VkCommandBuffer cmd);

private:
    static constexpr uint32_t kMaxStats = 190;   // counted draws a render (as marks)
    VkQueryPool statPool_ = VK_NULL_HANDLE;
    uint32_t statCount_[8] = {};
    const char* statNames_[8][kMaxStats] = {};
    bool statOpen_ = false;

    static constexpr uint32_t kMaxMarks = 190;   // (a forest's grass alone is a few dozen draws)
    static constexpr uint32_t kPerSlot = 2 + kMaxMarks;   // start, end, the marks

    render::VulkanContext& context_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double periodNs_ = 1.0;
    uint64_t validMask_ = ~0ull;
    double passLogMs_ = -1.0;               // BRO_SCENE_GPU_PASSES (< 0: passes untimed)
    // The slot's render's ms, read once its ticket completed (and its pass
    // and fragment lines logged).
    double harvest(uint32_t pair);

    uint32_t recordingPair_ = UINT32_MAX;   // slot begun in the open command buffer
    // Each slot's submitted render not read yet (its ticket, 0: none), and
    // the order they went in (lower first).
    uint64_t pendingTicket_[8] = {};
    uint64_t pendingOrder_[8] = {};
    uint64_t submits_ = 0;
    // Renders read (by begin, before their slot's queries were reset) and
    // not taken yet, oldest first.
    double ready_[8] = {};
    uint32_t readyCount_ = 0;
    // Each slot's marks: how many, and their names.
    uint32_t markCount_[8] = {};
    const char* markNames_[8][kMaxMarks] = {};
};

}  // namespace bro::scene::vk
