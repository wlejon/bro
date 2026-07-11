#pragma once

#include "js/message_queue.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::net { class NetService; }
namespace bro::util { class AssetMounts; }

namespace bro::js {

class Runtime;
class Timers;

/// A Web Worker running JavaScript on a dedicated thread.
/// Each worker gets its own JSRuntime + JSContext (separate heap, GC, job queue).
/// Workers have access to brokit APIs only — no DOM, no canvas, no window.
/// Communication is via postMessage/onmessage with structured clone semantics.
class Worker {
public:
    /// Create a worker. scriptPath is resolved relative to basePath.
    /// If netService is non-null, the worker installs bro.net bindings
    /// against it (own subscriber, polled on the worker's event loop).
    Worker(const std::string& scriptPath, const std::string& basePath,
           net::NetService* netService,
           const util::AssetMounts* mounts = nullptr);
    ~Worker();

    // Non-copyable, non-movable
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Start the worker thread. Must be called after construction.
    void start();

    /// Post a message from main thread → worker.
    /// Serializes value using structured clone. transferList is a JS Array
    /// of ArrayBuffers (or JS_UNDEFINED for no transfers).
    /// Returns true on success, false on error (JS exception thrown).
    bool postMessage(JSContext* mainCtx, JSValue value, JSValue transferList);

    /// Drain worker → main messages. For each message, calls the JS
    /// onmessage callback on the main-thread Worker object.
    /// Called by the Engine once per frame on the main thread.
    void drainMessages(JSContext* mainCtx);

    /// Terminate the worker thread (non-blocking signal + join).
    void terminate();

    /// Check if the worker thread is still alive.
    bool isAlive() const { return alive_.load(std::memory_order_acquire); }

    /// Tick rate in Hz — caps how fast the event loop iterates while it has
    /// pollable work or due timers (idle it blocks outright). Exposed through
    /// bro.server.tickrate inside the worker script. Safe to read/write
    /// across threads.
    void setTickRate(double hz);
    double tickRate() const { return tickRate_.load(std::memory_order_relaxed); }

    /// Seconds since the worker thread started executing its script.
    /// Returns 0 before the thread reaches its main loop.
    double uptimeSec() const;

    /// The JS Worker object on the main thread (DupValue'd).
    /// Set by the bindings after construction.
    JSValue jsObject = JS_UNDEFINED;

    /// Push a message onto the worker→main queue (called from worker thread).
    bool pushToMain(Message* msg) { return fromWorker_.push(msg); }

    /// Request the worker to stop (called from worker thread via self.close()).
    void requestClose() {
        terminated_.store(true, std::memory_order_release);
        wake();
    }

private:
    void threadFunc();
    void wake();

    std::string scriptPath_;
    std::string basePath_;
    net::NetService* netService_ = nullptr;
    const util::AssetMounts* mounts_ = nullptr;
    std::thread thread_;

    // Lock-free message queues (SPSC ring buffers)
    MessageQueue toWorker_;    // main → worker
    MessageQueue fromWorker_;  // worker → main

    // Wake signal for the worker's event loop. postMessage / terminate /
    // setTickRate bump wakeSeq_ under wakeM_ and notify; the loop blocks
    // here when it has nothing scheduled (no due timers, no in-flight
    // pollables) instead of waking at the tick rate to poll empty queues.
    std::mutex wakeM_;
    std::condition_variable wakeCv_;
    uint64_t wakeSeq_ = 0;  // guarded by wakeM_

    // terminated_ stays atomic: it is read by the QuickJS interrupt handler
    // in the middle of JS execution, where taking a lock is unwelcome.
    std::atomic<bool> terminated_{false};
    std::atomic<bool> alive_{false};

    // Event loop pacing cap. The loop never iterates faster than this;
    // beyond that it is deadline-driven (timers) or signal-driven
    // (messages), so the rate only throttles in-flight pollable work
    // (fetch/ws/net/async jobs) and back-to-back due timers. Default
    // 1000 Hz matches the legacy 1ms tick.
    std::atomic<double> tickRate_{1000.0};
    // Worker-thread-only: set at the top of threadFunc, read by uptimeSec().
    std::atomic<double> startTimeMs_{0.0};
};

// ---------------------------------------------------------------------------
// JS bindings
// ---------------------------------------------------------------------------

/// Install the Worker constructor on the global object.
/// If netService is non-null, workers created from this context will have
/// bro.net bindings installed on their JS context.
/// Must be called after brokit APIs are installed.
void installWorkerBindings(JSContext* ctx, const std::string& appBasePath,
                           net::NetService* netService,
                           const util::AssetMounts* mounts = nullptr);

/// Clean up worker bindings state for a context (call before freeing context).
void cleanupWorkerBindings(JSContext* ctx);

/// Drain messages from all workers on this context. Called once per frame.
void tickWorkers(JSContext* ctx);

}  // namespace bro::js
