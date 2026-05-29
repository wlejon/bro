#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace bro::engine {

// Lock-free single-producer / single-consumer ring of audio samples.
//
// PRODUCER (real-time audio thread): a broaudio mic-tap callback copies the
// resampled + AGC'd samples in. It never touches the model, the GPU, or the
// heap, so it can never block or stall mic capture no matter what other threads
// (e.g. a worker running an 8B LLM on CUDA) are doing on the device.
// CONSUMER (the AudioInference worker, or — headless — the calling thread):
// drains whatever has accumulated and feeds the model.
//
// Monotonic 64-bit positions; overflow (consumer stalled beyond the ring span)
// drops the newest samples, which at a frame or two of latency cannot happen in
// normal operation. One ring per tenant, held by shared_ptr and captured by the
// tap callback, so a late callback after stop()/re-register writes into its own
// (now unread) ring rather than racing a freshly installed one.
struct PcmRing {
    explicit PcmRing(std::size_t cap) : buf(cap, 0.0f) {}

    std::vector<float>    buf;
    std::atomic<uint64_t> writePos{0};
    std::atomic<uint64_t> readPos{0};

    // Producer (audio thread).
    void write(const float* s, int n);

    // Consumer (inference thread). Copies all available samples into `out` and
    // returns the count.
    int read(std::vector<float>& out);
};

// Engine-level audio-inference subsystem.
//
// Owns a background worker thread that pulls audio rings and runs neural-net
// models OFF the real-time audio thread AND OFF the main thread. This is the
// home for any model driven by live audio — wake word today, streaming STT and
// VAD later — so NN inference stops being a special case wedged onto whichever
// thread happened to deliver the samples.
//
// Threading mirrors bro::physics::PhysicsWorld exactly:
//   - startThread() spawns the worker in Windowed/Server mode. The main thread
//     wakes it once per frame with signalPump(); the worker drains the rings and
//     runs the models a frame or two behind, like the render pipeline reading a
//     slightly stale snapshot. The audio thread never wakes the worker (it stays
//     real-time pure) — only the main thread does.
//   - stepInline() runs the same pump synchronously on the calling thread for
//     Headless mode (no worker thread), keeping scripted/virtual-time tests
//     deterministic — the established headless convention (cf. PhysicsWorld::
//     stepInline, broaudio renderBlock).
//   - shutdown() joins the worker. Tasks are destroyed ON the worker thread, so
//     a model's destructor (and its CUDA frees) runs there, never on the main
//     thread.
//
// Tenants own their model, their input ring, and their own result delivery. The
// subsystem knows nothing about wake words or JS — it just runs each registered
// task's process(samples, n) callback on the inference thread. Result delivery
// back to the main thread is the tenant's concern (e.g. bro.wake publishes a
// fire count via an atomic that tickWake drains).
class AudioInference {
public:
    using ProcessFn = std::function<void(const float* samples, int n)>;
    using TaskId    = std::uint32_t;
    static constexpr TaskId kInvalidTask = 0;

    AudioInference();
    ~AudioInference();

    AudioInference(const AudioInference&)            = delete;
    AudioInference& operator=(const AudioInference&) = delete;

    // Spawn the worker thread. Call once, in Windowed/Server mode only. In
    // Headless mode leave it unspawned and drive the subsystem via stepInline().
    void startThread();

    // True once a worker thread is running (Windowed/Server).
    bool threaded() const { return thread_.joinable(); }

    // Register a tenant. The subsystem drains `ring` each pump and calls
    // `process(drained, n)` on the inference thread. The shared_ptr keeps the
    // ring alive for an in-flight pump even after removeTask(). Returns a handle
    // for removeTask(); never returns kInvalidTask. Thread-safe (lock-free
    // command queue to the worker, which is the sole owner of the task list).
    TaskId addTask(std::shared_ptr<PcmRing> ring, ProcessFn process);

    // Unregister a tenant. The worker destroys the task (dropping its model ref)
    // on its own thread. Safe to call with an unknown/stale id.
    void removeTask(TaskId id);

    // Main thread, once per frame: wake the worker to pump. No-op when not
    // threaded. Never blocks.
    void signalPump();

    // Headless: drain rings + run process() for every task on the calling
    // thread. No-op when threaded.
    void stepInline();

    // Signal the worker to stop and join it. Idempotent. Must run before the
    // audio engine and brotensor are torn down.
    void shutdown();

private:
    void workerFunc();
    void drainCommands();   // worker / inline: apply queued add/remove to tasks_
    void pumpTasks();       // worker / inline: drain each ring, run process()

    struct Task {
        TaskId                   id = kInvalidTask;
        std::shared_ptr<PcmRing> ring;
        ProcessFn                process;
    };

    // Command queue (main thread -> worker). Mutations to the task list happen
    // ONLY on the worker (single-owner), applied from these commands — so no
    // mutex is needed to guard tasks_ against the worker's iteration.
    struct Command {
        enum Type { Add, Remove } type = Add;
        TaskId                    id   = kInvalidTask;
        std::shared_ptr<PcmRing>  ring;       // Add only
        ProcessFn                 process;    // Add only
    };

    void pushCommand(Command* c);

    // Worker-thread state machine (std::atomic wait/notify — the physics
    // pattern). kPump requests a drain; kBusy is set while pumping; the worker
    // CAS-es back to kIdle unless a new kPump (or kShutdown) arrived meanwhile.
    enum State : std::uint32_t { kIdle = 0, kPump = 1, kBusy = 2, kShutdown = 3 };
    std::atomic<std::uint32_t> state_{kIdle};

    // Lock-free SPSC command ring of owning Command* (cf. js/message_queue.h).
    static constexpr std::size_t kCmdCap = 64;  // power of two; commands are rare
    Command*                        cmdSlots_[kCmdCap]{};
    alignas(64) std::atomic<std::size_t> cmdHead_{0};  // consumer (worker)
    alignas(64) std::atomic<std::size_t> cmdTail_{0};  // producer (main)

    std::vector<Task>          tasks_;     // worker-owned
    std::vector<float>         scratch_;   // worker/inline drain buffer
    std::atomic<TaskId>        nextId_{1};
    std::thread                thread_;
};

}  // namespace bro::engine
