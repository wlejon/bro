#pragma once
// GPU time of the scene's renders, from timestamp queries written at the start
// and end of each render's command buffer: what perf.gpuFrameMs() reports.
// One set of queries per frame slot, so a set is reset only once the frame
// ring has waited for its previous use. Nothing waits until a caller asks.
//
// Once timePasses() has been called (perf.gpuPasses() calls it), each pass of
// the frame graph also writes a stamp as it ends (mark()), and a render read
// keeps its time pass by pass (lastPasses): where a frame's time went,
// without a profiler attached. Before that no mark writes anything.
//
// A frame's compute stream on the async compute queue (SceneFrame::
// computeStream) is stamped in a query pool of its own: its start and end,
// and its marks (a mark recorded into that command buffer goes there), kept
// as the render's compute passes, led by "start": the stream's start after
// the graphics' (negative: before it). The frame's time is then wall time on the
// GPU: from the earlier of the two starts to the later end. A frame whose
// start overlaps the previous frame's GPU work (the previous end read)
// counts from that end instead, so overlapped frames do not count the
// overlap twice.
//
// Spans are stamped always, timed passes or not: a numbered stretch of a
// render (the world service's jobs of one kind, scene/vulkan/world/
// scene_world_service.h) with the work it held (its jobs' units), on the
// graphics queue or on the async compute stream. A span may run in a few
// segments (a kind's jobs drawn in one pass and copied in another): its time
// is theirs summed, its units theirs. Every render read, oldest first, is kept with
// its spans for takeFrames(), apart from takeMs(): the world service reads
// its frames there whatever perf.gpuFrameMs reads.
#include "render/vulkan_context.h"

#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bro::scene::vk {

class SceneGpuTimer {
public:
    /// A render's passes: each mark's name and its milliseconds since the
    /// mark before it (a name marked more than once, summed), then "rest"
    /// (the last mark to the end).
    using Passes = std::vector<std::pair<std::string, double>>;
    static constexpr uint32_t kMaxSpans = 12;
    static constexpr uint32_t kSpanSegments = 4;   // a span's segments a render
    /// A render read: its milliseconds (as takeMs reports them) and its spans.
    struct Frame {
        uint64_t order = 0;   // its place among the timed renders submitted (1 the first)
        double ms = 0.0;
        struct Span {
            uint32_t id = 0;
            double units = 0.0;   // the work it held (its jobs' units)
            double ms = 0.0;
            bool apart = false;   // on the async compute queue
        };
        std::vector<Span> spans;
    };

    explicit SceneGpuTimer(render::VulkanContext& context) : context_(context) {}
    ~SceneGpuTimer();
    SceneGpuTimer(const SceneGpuTimer&) = delete;
    SceneGpuTimer& operator=(const SceneGpuTimer&) = delete;

    /// Every timer's renders from now on write their passes' marks
    /// (process-wide; there is no turning it off).
    static void timePasses();

    /// False when the queue cannot write timestamps (the timer then stays off).
    bool init();
    /// Reset this frame's queries and write the start stamp (outside rendering).
    void begin(VkCommandBuffer cmd);
    /// The end of the part of the render called `name` (a string that
    /// outlives the timer); a no-op unless passes are being timed.
    void mark(VkCommandBuffer cmd, const char* name);
    /// Write the end stamp; the render is timed once `ticket` completes.
    void end(VkCommandBuffer cmd);
    /// The render's compute stream on the async compute queue, begun (reset
    /// its queries, write its start) and ending (its end stamp). A no-op
    /// when the render is untimed or that queue cannot write timestamps.
    void beginCompute(VkCommandBuffer cmd);
    void endCompute(VkCommandBuffer cmd);
    /// A segment of span `id` (below kMaxSpans; up to kSpanSegments a
    /// render, all on one queue) begins and ends in `cmd`: the render's
    /// graphics, or (`apart`) its compute stream on the async compute queue,
    /// holding `units` of the span's work. False, and nothing written, when
    /// the render or that stream is untimed or the span has no segment left.
    bool beginSpan(VkCommandBuffer cmd, uint32_t id, bool apart);
    void endSpan(VkCommandBuffer cmd, uint32_t id, bool apart, double units);
    /// `ticket` is the render's last graphics submission, which waits for its
    /// compute: once it completes, both are done.
    void submitted(uint64_t ticket);
    /// The renders read since the last call (oldest first, at most 16 kept),
    /// reading those that have finished without waiting.
    void takeFrames(std::vector<Frame>& out);
    /// Timed renders submitted so far: the open one, timed, will be the next.
    uint64_t submittedRenders() const { return submits_; }
    /// Milliseconds of the last submitted render, waiting for it (its own
    /// ticket); -1 when nothing was timed since the last call. Not `wait`:
    /// the oldest render that has finished and was not taken yet, -1 for
    /// none, so a caller reading every frame reads each render once, a frame
    /// or so behind, and its frames overlap the GPU's.
    double takeMs(bool wait = true);
    /// The passes of the last render read that had marks (reading the
    /// submitted ones first, waiting for the newest), and its compute
    /// stream's (`computed` false when it had none apart). False when no
    /// render with marks has been read.
    bool lastPasses(Passes& passes, Passes& compute, bool& computed);

private:
    // (A scene's grass alone is a few dozen draws, and a large scene's
    // opaque and shadow passes can take hundreds.)
    static constexpr uint32_t kMaxMarks = 512;
    static constexpr uint32_t kSpanBase = 2 + kMaxMarks;
    // start, end, the marks, the spans' segments
    static constexpr uint32_t kPerSlot = kSpanBase + 2 * kMaxSpans * kSpanSegments;

    render::VulkanContext& context_;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    double periodNs_ = 1.0;
    uint64_t validMask_ = ~0ull;
    // The slot's render's ms, read once its ticket completed (and its
    // passes kept).
    double harvest(uint32_t pair);
    // The submitted renders read into ready_, oldest first; not `wait`ing,
    // up to the first still on the GPU.
    void drain(bool wait);

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
    // Each slot's spans: those written (on the graphics queue, on the
    // compute stream), the one with a segment open, and each one's segments
    // and units.
    uint32_t spanMask_[8] = {};
    uint32_t spanApart_[8] = {};
    uint32_t spanOpen_[8] = {};
    uint32_t spanSegments_[8][kMaxSpans] = {};
    double spanUnits_[8][kMaxSpans] = {};
    // The renders read for takeFrames, oldest first.
    std::vector<Frame> frames_;
    // Each slot's marks: how many, and their names.
    uint32_t markCount_[8] = {};
    const char* markNames_[8][kMaxMarks] = {};
    // The last render read with marks: its passes.
    Passes lastPasses_, lastCompute_;
    bool lastComputed_ = false, havePasses_ = false;

    // The compute stream's stamps (start, end, marks), a slot as the
    // graphics' (kPerSlot each), when the async compute queue has timestamps.
    VkQueryPool computePool_ = VK_NULL_HANDLE;
    uint64_t computeMask_ = ~0ull;
    VkCommandBuffer computeCmd_ = VK_NULL_HANDLE;   // the open render's compute stream
    bool computeTimed_[8] = {};
    uint32_t computeMarkCount_[8] = {};
    const char* computeMarkNames_[8][kMaxMarks] = {};
    uint64_t lastEnd_ = 0;   // the end stamp of the last render read
};

}  // namespace bro::scene::vk
