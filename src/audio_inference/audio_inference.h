#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
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
// Threading:
//   - startThread() spawns the worker in Windowed/Server mode. The worker is
//     SELF-PACED: it drains the rings and runs the models on its own short clock,
//     independent of the render frame loop. The audio thread only writes a
//     tenant's lock-free PcmRing (it stays real-time pure, never signals); the
//     main thread only delivers results (tickWake). So inference cadence does not
//     track frame rate — rendering hiccups can neither throttle detection nor
//     drop audio (the ~2 s ring covers a sub-second stall), and the worker never
//     burdens a frame. (This is why it does NOT mirror PhysicsWorld's per-frame
//     signalStep: a real-time audio model must not be gated on the frame loop.)
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
    // for removeTask(); never returns kInvalidTask. Thread-safe (locked command
    // queue to the worker, which is the sole owner of the task list).
    TaskId addTask(std::shared_ptr<PcmRing> ring, ProcessFn process);

    // Unregister a tenant. The worker destroys the task (dropping its model ref)
    // on its own thread. Safe to call with an unknown/stale id.
    //
    // BARRIER (threaded mode): blocks until the worker has applied the removal,
    // which guarantees any in-flight pump still running the removed task's
    // closure has completed. So once this returns the caller may safely mutate
    // (or destroy) that tenant's model from the main thread — there is no longer
    // any worker access to it. Must be called from the main thread, never from a
    // task's process() callback (it would deadlock against its own worker).
    void removeTask(TaskId id);

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

    // Command queue (main thread -> worker). Mutations to the task list still
    // happen ONLY on the worker (single-owner), applied from these commands —
    // the mutex guards just the queue, the counters, and the shutdown flag,
    // never tasks_ or a running pump. The audio thread stays lock-free (it
    // only ever writes a PcmRing).
    struct Command {
        enum Type { Add, Remove } type = Add;
        TaskId                    id   = kInvalidTask;
        std::shared_ptr<PcmRing>  ring;       // Add only
        ProcessFn                 process;    // Add only
    };

    // Guarded by m_. cmdsPushed_/cmdsApplied_ implement the removeTask
    // barrier: a waiter records the count at push and blocks on cv_ until
    // the worker's applied count catches up (commands apply in FIFO order).
    // The worker's pacing wait also sits on cv_, so a pushed command wakes
    // it immediately instead of waiting out the drain interval.
    std::mutex                 m_;
    std::condition_variable    cv_;
    std::deque<Command>        cmdQueue_;
    std::uint64_t              cmdsPushed_  = 0;
    std::uint64_t              cmdsApplied_ = 0;
    bool                       shutdown_    = false;

    std::vector<Task>          tasks_;     // worker-owned
    std::vector<float>         scratch_;   // worker/inline drain buffer
    std::atomic<TaskId>        nextId_{1};
    std::thread                thread_;
};

}  // namespace bro::engine
