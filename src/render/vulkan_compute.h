#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace bro::render {

class VulkanContext;
class VulkanCompute;
class VulkanQueue;
struct ComputeOwner;

/// A device-local storage buffer that compute shaders reach by its device
/// address (VulkanComputeFeatures::bufferDeviceAddress): no descriptor sets.
/// Made by VulkanCompute::createBuffer; it reads zero until written. Also
/// usable as a vertex, index or indirect buffer and as a transfer source or
/// destination. Released once the GPU is done with it.
class ComputeBuffer {
public:
    ~ComputeBuffer();
    ComputeBuffer(const ComputeBuffer&) = delete;
    ComputeBuffer& operator=(const ComputeBuffer&) = delete;

    VkBuffer buffer() const { return buffer_; }
    uint64_t address() const { return address_; }
    /// The bytes asked for.
    VkDeviceSize size() const { return size_; }
    /// The bytes behind address(): size() rounded up to 256, at least 256
    /// (an empty buffer still has room for a shader's discarded access).
    VkDeviceSize capacity() const { return capacity_; }

    /// A log of which of the buffer's `layers` equal parts its writer has
    /// rewritten, for a consumer that copies it part by part (a texture
    /// array filled from it): each markLayers() stamps the parts it names
    /// with the next count, so a consumer that copied everything up to count
    /// c recopies exactly the parts stamped after c. A buffer never marked
    /// has no log, and is copied whole. Marking with another part count
    /// starts a new log, every part stamped. Page thread only, like writes.
    void markLayers(std::span<const uint32_t> parts, uint32_t layers);
    /// The log's count (0: never marked) and its part count.
    uint64_t layerCount() const { return layerCount_; }
    uint32_t layerParts() const { return static_cast<uint32_t>(layerStamps_.size()); }
    /// The count at which part `layer` was last marked.
    uint64_t layerStamp(uint32_t layer) const { return layer < layerStamps_.size() ? layerStamps_[layer] : 0; }

    /// Release the buffer's GPU memory once in-flight work finishes.
    void releaseDeferred();

private:
    friend class VulkanCompute;
    ComputeBuffer() = default;
    void release();

    std::vector<uint64_t> layerStamps_;
    uint64_t layerCount_ = 0;
    std::shared_ptr<ComputeOwner> owner_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    uint64_t allocId_ = 0;
    uint64_t address_ = 0;
    VkDeviceSize size_ = 0;
    VkDeviceSize capacity_ = 0;
};

/// What a ComputePipeline is made from: one SPIR-V compute entry point whose
/// only resources are a push-constant block (buffers are passed by address).
struct ComputePipelineDesc {
    std::span<const uint32_t> spirv;
    std::string entry = "main";
    uint32_t pushConstantBytes = 0;
    uint32_t localSize[3] = {1, 1, 1};
    /// The local size is given as specialization constants 0..2 (else it is
    /// the module's own LocalSize).
    bool localSizeSpecConstants = false;
    /// What its dispatches' GPU time is reported under (VulkanCompute::takeTimes).
    std::string label;
};

/// A compute pipeline's driver objects, made on any thread
/// (VulkanCompute::buildPipeline: the driver's compile, which may take a
/// while, kept off the frame) and handed to the page thread, which adopts
/// them (adoptPipeline) into a ComputePipeline it owns from then on. Nothing
/// the page thread keeps is touched while it is built. Destroyed with it
/// when never adopted. Move-only.
struct ComputePipelineBuild {
    VkDevice device = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;   // null: failed, `error` says why
    uint32_t pushBytes = 0;
    std::string label;
    std::string error;

    ComputePipelineBuild() = default;
    ComputePipelineBuild(ComputePipelineBuild&& o) noexcept;
    ComputePipelineBuild& operator=(ComputePipelineBuild&& o) noexcept;
    ComputePipelineBuild(const ComputePipelineBuild&) = delete;
    ~ComputePipelineBuild();
};

/// A byte range of buffer memory an operation reads or writes, by device
/// address: what VulkanCompute orders recorded work by. An operation waits
/// only for earlier ones it conflicts with: a read for the writes to its
/// range, a write for the reads and writes to it.
struct ComputeAccess {
    uint64_t address = 0;
    uint64_t bytes = 0;
    bool write = false;
};
/// Every byte, written: an operation whose accesses are not known, ordered
/// after everything recorded before it and before everything after it.
inline constexpr ComputeAccess kAnyAccess[1] = {{0, ~0ull, true}};

/// The GPU time of a label's dispatches.
struct ComputeTime {
    std::string label;
    double ms = 0.0;
    uint32_t dispatches = 0;
    /// Run in the background stream (VulkanCompute::beginBackground): beside
    /// the frames, so not part of any frame's length; its cost to a frame is
    /// what it slows the frame's own work by.
    bool background = false;
};

/// A compute pipeline with a push-constant-only layout. Released once the
/// GPU is done with it.
class ComputePipeline {
public:
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    uint32_t pushConstantBytes() const { return pushBytes_; }
    VkPipeline pipeline() const { return pipeline_; }

private:
    friend class VulkanCompute;
    ComputePipeline() = default;
    void release();

    std::shared_ptr<ComputeOwner> owner_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    uint32_t pushBytes_ = 0;
    std::shared_ptr<const std::string> label_;
};

/// Compute work on the graphics queue, in submission order with the frames'
/// draws: buffer writes, dispatches and readbacks recorded into one open
/// command buffer of the current frame, which is submitted when something
/// needs its results: a readback, a consumer about to submit work that reads
/// a compute buffer (flush()), or the frame's end. Each recorded operation
/// sees every earlier one's writes; work submitted after a flush sees them
/// all (vertex, index, indirect, shader and transfer reads alike).
/// Operations are ordered by the buffer ranges they touch (ComputeAccess):
/// a barrier goes between two only where one writes what the other reads
/// or writes, so a run of independent dispatches overlaps on the GPU.
///
/// Owned by VulkanContext (compute()). Main thread.
/// Bytes of a buffer copied into host memory without waiting: the copy is
/// recorded after everything recorded before it and submitted at once, and
/// the bytes are there once ready() (the GPU has passed it). Made by
/// VulkanCompute::readLater.
class ComputeReadback {
public:
    ~ComputeReadback();
    ComputeReadback(const ComputeReadback&) = delete;
    ComputeReadback& operator=(const ComputeReadback&) = delete;

    bool ready() const;
    /// The copied bytes once ready(); size() of them.
    const void* data() const { return mapped_; }
    VkDeviceSize size() const { return size_; }

private:
    friend class VulkanCompute;
    ComputeReadback() = default;
    void release();

    std::shared_ptr<ComputeOwner> owner_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    uint64_t allocId_ = 0;
    void* mapped_ = nullptr;
    VkDeviceSize size_ = 0;
    uint64_t ticket_ = 0;
    bool background_ = false;   // its ticket is the background stream's
};

/// The GPU time of the work recorded between VulkanCompute::beginSpan and
/// endSpan, from the end of everything submitted before it to the end of
/// that work. Made by beginSpan; there once ready(), waiting for nothing.
class ComputeSpan {
public:
    ~ComputeSpan();
    ComputeSpan(const ComputeSpan&) = delete;
    ComputeSpan& operator=(const ComputeSpan&) = delete;

    bool ready() const;
    /// Its GPU milliseconds once ready(), else negative.
    double ms() const;

private:
    friend class VulkanCompute;
    ComputeSpan() = default;

    std::shared_ptr<ComputeOwner> owner_;
    uint32_t query_ = 0;
    uint64_t ticket_ = 0;
    bool background_ = false;
    mutable double ms_ = -1.0;
};

class VulkanCompute {
public:
    VulkanCompute() = default;
    ~VulkanCompute();
    VulkanCompute(const VulkanCompute&) = delete;
    VulkanCompute& operator=(const VulkanCompute&) = delete;

    bool init(VulkanContext& context);
    /// Releases every buffer and pipeline still alive (the device is idle);
    /// a handle that outlives it holds nothing.
    void shutdown();

    /// The device reaches buffers by address and computes in 64-bit integers:
    /// what every buffer and pipeline here needs.
    bool available() const;

    std::shared_ptr<ComputeBuffer> createBuffer(VkDeviceSize size, std::string* error = nullptr);
    std::shared_ptr<ComputePipeline> createPipeline(const ComputePipelineDesc& desc, std::string* error = nullptr);
    /// createPipeline in two halves: the driver's objects, on any thread
    /// (it reads only the device), then adopted here, on the page thread.
    ComputePipelineBuild buildPipeline(const ComputePipelineDesc& desc) const;
    std::shared_ptr<ComputePipeline> adoptPipeline(ComputePipelineBuild&& build, std::string* error = nullptr);

    /// Copy `size` bytes from `data` (now) to `buffer` at `offset`.
    bool write(ComputeBuffer& buffer, VkDeviceSize offset, const void* data, VkDeviceSize size);
    /// Dispatch `groups` workgroups of `pipeline` with `push` as its
    /// push-constant block (pushConstantBytes() long). `access` is every
    /// buffer range it reads or writes (the default: unknown, ordered
    /// against everything).
    bool dispatch(const ComputePipeline& pipeline, std::span<const uint8_t> push, const uint32_t groups[3],
                  std::span<const ComputeAccess> access = kAnyAccess);
    /// Copy `size` bytes of `buffer` at `offset` into `out`, after every
    /// operation recorded so far: submits them and waits for that ticket.
    bool read(const ComputeBuffer& buffer, VkDeviceSize offset, void* out, VkDeviceSize size);
    /// The same copy, not waited for: null on failure.
    std::shared_ptr<ComputeReadback> readLater(const ComputeBuffer& buffer, VkDeviceSize offset, VkDeviceSize size);
    /// Record `fn`'s commands in the graphics stream, after every operation
    /// recorded so far (their writes visible to its compute and transfer
    /// stages); the operations recorded after it wait for its compute and
    /// transfer writes. Work of the render module's own over the stream's
    /// buffers (a 'kernel' canvas's present). False in the background stream.
    bool recordGraphics(const std::function<void(VkCommandBuffer)>& fn);

    /// Submit what is recorded. Returns its ticket on the graphics queue, or
    /// the last submission's when nothing was recorded since.
    uint64_t flush();
    /// Flush open graphics work (when not in background) and reclaim completed
    /// deferred frees and transient frame buffers.
    void reclaim();

    /// Record into the background stream until endBackground(): work for
    /// the gap between frames rather than inside one, on the async compute
    /// queue where the device has one (else the graphics queue, in order,
    /// the same contract). What is recorded is held until the frame's GPU
    /// work is all submitted (releaseBackground: the engine calls it after
    /// the frame's present, and at the latest when the next frame begins),
    /// then starts after everything submitted to the graphics queue so far:
    /// it runs in the time the frame leaves before the next one, not beside
    /// its draws (sharing the GPU with them, a lone planet tile of 0.33 ms
    /// slowed a frame's render by more than its own length). It may rewrite
    /// what an earlier frame draws from, and is in order with earlier
    /// background work; nothing on the graphics queue waits for it unless
    /// told to (acquireBackground). For work whose results are wanted
    /// frames later: a tile cache's production. Its tickets, and those of
    /// readbacks and spans begun in it, are the stream's own serials, which
    /// complete once the work they mark is submitted and done.
    void beginBackground();
    /// Go back to the graphics stream. The ticket of the work recorded
    /// (backgroundComplete), or of the last background work when nothing
    /// was recorded.
    uint64_t endBackground();
    bool backgroundComplete(uint64_t ticket) const;
    /// Submit the background work held: called once the frame's GPU work is
    /// all submitted.
    void releaseBackground();
    /// releaseBackground and wait for every background submission to finish:
    /// a frame's refinement run in the gap before the next (headless's
    /// perf.measure, which has no vblank to wait for).
    void finishBackground();
    /// GPU milliseconds of the background submissions finished since the
    /// last call, each from the start of its work (after the graphics it
    /// waits for) to its end, and of the newest one alone (negative before
    /// any). Waits for nothing.
    double takeBackgroundMs();
    double lastBackgroundMs();
    /// Between beginBackground and endBackground.
    bool inBackground() const { return background_; }
    /// The graphics queue's next submission from here waits for background
    /// ticket `ticket`: what it and everything after it on the graphics
    /// queue read of that work is there. Called once the ticket is complete
    /// it costs nothing.
    void acquireBackground(uint64_t ticket);

    /// Time every dispatch on the GPU from now on (timestamps either side of
    /// it), or stop. Off by default: it costs two queries a dispatch.
    /// Dispatches that run together (nothing orders them) each count the
    /// time they share, so their sum can pass the time they took.
    void setTiming(bool on);
    bool timing() const { return timing_; }
    /// The GPU time of the dispatches the GPU has finished since the last
    /// call, summed by their pipelines' labels. Waits for nothing.
    std::vector<ComputeTime> takeTimes();

    /// Time the work recorded from now to endSpan(): null when the device
    /// has no timestamps or too many spans are alive.
    std::shared_ptr<ComputeSpan> beginSpan();
    /// End the span and submit what is recorded.
    void endSpan(ComputeSpan& span);

private:
    // What a stream records into: the graphics one, and the background one.
    // The buffer ranges the operations recorded since the open buffer's
    // last barrier read and wrote: what the next operation may have to wait
    // for (ComputeAccess).
    struct Range {
        uint64_t begin = 0;
        uint64_t end = 0;
    };
    struct Stream {
        VkCommandBuffer open = VK_NULL_HANDLE;
        std::vector<Range> reads;
        std::vector<Range> writes;
        uint32_t openDispatches = 0;
        uint64_t lastTicket = 0;
        uint64_t prevBatchTicket = 0;
    };
    // The stream recording now. The background one is held to the frame's
    // end on a device without an async queue too, and then submitted to the
    // graphics queue.
    Stream& current() { return recordingBackground() ? backgroundStream_ : graphicsStream_; }
    bool async() const;
    bool recordingBackground() const { return background_; }
    uint64_t flushStream(Stream& s, bool isBackground);
    VulkanQueue& queueFor(bool isBackground) const;

    // The open command buffer of the current stream, after a barrier for
    // whatever recorded before `access` conflicts with; `access` is then
    // what the operation recorded next touches.
    VkCommandBuffer commands(std::span<const ComputeAccess> access = kAnyAccess);
    void barrier(Stream& s);
    // The timestamp period and valid bits into owner_, once: false when the
    // queue writes no timestamps.
    bool timestamps();
    // Finished dispatches' stamps into times_.
    void collectTimes();
    // A serial for the background work recorded so far (or the last one,
    // when the stream holds nothing).
    uint64_t markBackground();
    // Finished background submissions' stamps into backgroundMs_.
    void collectBackgroundStamps();
    struct BackgroundStamp {
        uint32_t query = 0;
        uint64_t ticket = 0;
    };
    VkQueryPool backgroundPool_ = VK_NULL_HANDLE;
    uint32_t backgroundQuery_ = 0;       // the next pair in the ring
    bool backgroundStamped_ = false;     // the open background buffer began with a stamp
    uint32_t backgroundStampQuery_ = 0;  // ... at this pair
    std::deque<BackgroundStamp> backgroundStamps_;
    double backgroundMs_ = 0.0;
    double lastBackgroundMs_ = -1.0;

    // A timed dispatch: its pair of queries, and the ticket of the
    // submission it went in (0 while still recorded).
    struct TimedDispatch {
        std::shared_ptr<const std::string> label;
        uint32_t query = 0;
        uint64_t ticket = 0;
        bool background = false;
    };
    bool timing_ = false;
    VkQueryPool timePool_ = VK_NULL_HANDLE;
    uint32_t nextQuery_ = 0;
    std::deque<TimedDispatch> timed_;
    std::vector<ComputeTime> times_;

    VulkanContext* context_ = nullptr;
    std::shared_ptr<ComputeOwner> owner_;
    uint64_t frameHook_ = 0;
    Stream graphicsStream_;
    Stream backgroundStream_;
    bool background_ = false;      // between beginBackground and endBackground
    uint64_t acquire_ = 0;         // background ticket the next graphics submission waits for
    uint64_t acquired_ = 0;        // the last one a graphics submission waited for

    // Host-visible destination of readbacks, grown as needed.
    VkBuffer readback_ = VK_NULL_HANDLE;
    uint64_t readbackAlloc_ = 0;
    void* readbackMapped_ = nullptr;
    VkDeviceSize readbackSize_ = 0;
};

/// What a buffer or pipeline was made by: the context while it lives, and
/// the handles still alive, which shutdown releases.
struct ComputeOwner {
    VulkanContext* context = nullptr;
    std::unordered_set<ComputeBuffer*> buffers;
    std::unordered_set<ComputePipeline*> pipelines;
    std::unordered_set<ComputeReadback*> readbacks;
    VkDeviceSize deferredBytes = 0;
    // Timestamps: ns a tick (0 until known, negative when there are none)
    // and the valid bits' mask.
    double timestampNs = 0.0;
    uint64_t timestampMask = 0;
    // Spans' queries, two a span, and the pairs no span holds.
    VkQueryPool spanPool = VK_NULL_HANDLE;
    std::vector<uint32_t> freeSpans;

    // The background stream's serials (VulkanCompute::beginBackground): a
    // mark of the work recorded up to it, which becomes a queue ticket when
    // the stream is submitted. Submissions oldest first, by the newest
    // serial each carries.
    struct BackgroundSubmit {
        uint64_t lastSerial = 0;
        uint64_t ticket = 0;   // 0: the submission failed (nothing to wait for)
    };
    std::deque<BackgroundSubmit> backgroundSubmits;
    uint64_t backgroundMarked = 0;     // the newest serial handed out
    uint64_t backgroundSubmitted = 0;  // the newest one submitted
    uint64_t backgroundCompleted = 0;  // the newest one the GPU has done
    bool backgroundSerialComplete(uint64_t serial);
    // The queue ticket of `serial`'s submission: 0 once it is done (or when
    // it is not submitted yet).
    uint64_t backgroundTicket(uint64_t serial);
};

} // namespace bro::render
